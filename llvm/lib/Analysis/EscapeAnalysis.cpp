//==- EscapeAnalysis.cpp - Generic Escape Analysis Implementation --==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the generic Escape Analysis interface.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/EscapeAnalysis.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Analysis/SingleThreaded.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Signals.h"

#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace llvm;


// Which notion of "escaped" an access is judged by.
//
// The default is per program point: an access is elided if the object has not
// escaped on any path from the function entry to the access's block. That
// elides a write made before the object's address is published in a later
// block, and with it the report stock TSan would give on that object (the
// publication itself is still reported). The paper's escape proposition is
// stated per object -- an access is exempt only if no location it touches is
// marked escaping -- which is the flow-insensitive notion this flag selects:
// an object that escapes anywhere in the function is escaped everywhere in it.
// Measurement switches for two fail-closed rules, so their cost can be
// attributed. Both default on; turning either off is unsound.
static cl::opt<bool> ClEAUnknownTop(
    "tsan-ea-unknown-operand-is-top", cl::init(true), cl::Hidden,
    cl::desc("An operand the object walk cannot resolve, once stored, passed "
             "or returned, makes every object escaped (UNSOUND if off)"));
static cl::opt<bool> ClEAUnseenPointeeEscapes(
    "tsan-ea-unseen-pointee-escapes", cl::init(true), cl::Hidden,
    cl::desc("A pointer loaded from a slot with no recorded pointee is "
             "escaped (UNSOUND if off)"));
static cl::opt<bool> ClEAFlowInsensitive(
    "tsan-ea-flow-insensitive", cl::init(false), cl::Hidden,
    cl::desc("Treat an object that escapes anywhere in a function as escaped "
             "at every point of it (the per-object notion), instead of only "
             "from the point of escape on"));

#define DEBUG_TYPE "ea"

STATISTIC(NumEAUnknownOperands,
          "Escape analysis: operands the object walk could not resolve");
STATISTIC(NumEAUnresolvedIntToPtr, "  unresolved: inttoptr");
STATISTIC(NumEAUnresolvedNull, "  unresolved: null");
STATISTIC(NumEAUnresolvedUndef, "  unresolved: undef/poison");
STATISTIC(NumEAUnresolvedConstExpr, "  unresolved: constant expression");
STATISTIC(NumEAUnresolvedPHI, "  unresolved: phi");
STATISTIC(NumEAUnresolvedSelect, "  unresolved: select");
STATISTIC(NumEAUnresolvedLoad, "  unresolved: load");
STATISTIC(NumEAUnresolvedExtractValue, "  unresolved: extractvalue");
STATISTIC(NumEAUnresolvedGEP, "  unresolved: gep (lookup limit)");
STATISTIC(NumEAUnresolvedOther, "  unresolved: other");
STATISTIC(NumEAUnseenPointeeLoads,
          "Escape analysis: queries answered escaped because the loaded "
          "pointer's slot had no recorded pointee");

#define PRINT_ESCAPING_CALLEES "ea-escaping-callees"

//===----------------------------------------------------------------------===//
// For debug
//===----------------------------------------------------------------------===//

void EscapeAnalysisInfo::printEscReason(EscReasonTy EscReason) {
  if (EscReason[0]) dbgs() << "GPTR_ALIASING ";
  if (EscReason[1]) dbgs() << "PTR_ARG_ALIASING ";
  if (EscReason[2]) dbgs() << "PASSING_TO_CALL ";
  if (EscReason[3]) dbgs() << "RET_PTR ";
  if (EscReason[4]) dbgs() << "VOLATILE ";
  if (EscReason[5]) dbgs() << "ESCAPED_CALL ";
  if (EscReason[6]) dbgs() << "OTHER ";
  if (EscReason[7]) dbgs() << "INVALID ";
  dbgs() << "\n";
}

/// Print IPA Function Escape Information
[[maybe_unused]] static void
printIPAFuncEscInfo(const EscapeAnalysisInfo::IPABottomTopMap *IPAFuncEscInfo,
                    raw_ostream &OS) {
  OS << "IPA Function Escape Analysis Information:\n";
  if (IPAFuncEscInfo == nullptr)
    return;

  for (const auto &FuncInfo : *IPAFuncEscInfo) {
    const auto &Info = FuncInfo.second;
    const Function *F = FuncInfo.first;
    OS << "Function: " << F->getName() << "\n";

    if (!Info.ArgEscapes.empty()) {
      OS << "Argument Escape Reasons:\n";
      for (const auto &ArgInfo : Info.ArgEscapes) {
        OS << "  Arg Index: " << ArgInfo.first << " - Escape Reasons: ";
        EscapeAnalysisInfo::printEscReason(ArgInfo.second);
        OS << "\n";
      }
    }
    OS << "Return Value Escapes: " << (Info.IsRetEscape ? "Yes" : "No") << "\n";
  }
}

void EscapeAnalysisGlobalInfo::printSCC(
    const std::vector<CallGraphNode *> &SCC) {
  dbgs() << "SCC: " << SCC.size() << "\n";
  for (const CallGraphNode *CGN : SCC) {
    if (CGN->getFunction())
      dbgs() << "\t" << CGN->getFunction()->getName() << "\n";
  }
}

static std::string dbgPathToStr(const FieldPathTy &Path) {
  if (Path == EmptyFieldPath)
    return "";

  std::string Result;
  raw_string_ostream RSO(Result);
  RSO << " | Path: ";
  for (size_t i = 0; i < Path.size(); ++i) {
    if (i != 0)
      RSO << " ";
    RSO << Path[i];
  }
  RSO.flush();
  return Result;
}

static std::string dbgObjToStr(const Value *Obj) {
  std::string Output;
  raw_string_ostream OS(Output);
  if (const auto *F = dyn_cast<Function>(Obj))
    OS << F->getName() << "\t";
  else if (const auto *BB = dyn_cast<BasicBlock>(Obj))
    OS << BB->getName() << "\t";
  else
    OS << *Obj << "\t";
  OS.flush();
  return Output;
}

static void dbgPrintAliasCand(const SmallVectorImpl<UnderlObjTy> &UnderlObjs) {
  for (const auto &A : UnderlObjs)
    dbgs() << "\tAlias candidate: " << *A.Obj << "\n";
}

void EscapeAnalysisGlobalInfo::printArgEscStatus() const {
  dbgs() << "\nIsArgEscapedFromCalls:\n";
  for (const auto &Entry : *IPATopDownArgEscInfo) {
    const Function *F = Entry.first;
    const SmallVector<bool> &EscapedArgs = Entry.second;
    dbgs() << "\tFunction: " << F->getName() << "\n";
    for (unsigned ArgIdx = 0; ArgIdx < EscapedArgs.size(); ++ArgIdx) {
      dbgs() << "\t\tArg " << ArgIdx << ": "
             << (EscapedArgs[ArgIdx] ? "YES" : "NO") << "\n";
    }
  }
  dbgs() << "\n";
}

//===----------------------------------------------------------------------===//
// Utils
//===----------------------------------------------------------------------===//

static bool isLocalAndExactFunc(const Function *F) {
  return F && !F->isDeclaration() && F->isDefinitionExact();
}

static bool isPointerArgument(const Value *V) {
  return isa<Argument>(V) && V->getType()->isPointerTy();
}

//===----------------------------------------------------------------------===//
// Alias relation
//===----------------------------------------------------------------------===//

/// If instruction creates an alias to the object which has escaped before
/// or escapes "by definition" (e.g. pointer function argument,
/// global pointer), then that's not just aliasing, but escaping as well
void EscapeAnalysisInfo::EscapeState::checkAndUpdEscStatus(
    const ObjAndPath &Pointer, const ObjAndPath &Pointee,
    const EscapeAnalysisInfo *EAI, const Instruction *I) {
  LLVM_DEBUG(dbgs() << "\tcheckAndUpdEscStatus: " << *Pointer.Obj << " --> "
                    << *Pointee.Obj << "\n");
  // The pointer's memory may be shared for two reasons: it is external -- a
  // global, an argument, a call result -- or the analysis has already found
  // it escaped (published earlier in this function). Only the first was
  // consulted, so `store ptr %s, @sink; store ptr %x, ptr %s` left x local.
  // Either way the pointee escapes, and so does everything it points to.
  EscReasonTy PointerReason = EAI->getExtObjStatusIPA(Pointer.Obj);
  PointerReason |= getEscReason(Pointer);
  if (PointerReason.any())
    addEscObj(Pointee, PointerReason, I);

  // Assigning to structures
  if (const auto *Alloca = dyn_cast<AllocaInst>(Pointer.Obj);
      Alloca && Alloca->getAllocatedType()->isStructTy()) {
    if (const auto Reason = getEscReason(Pointee); Reason.any())
      addEscObjOrReason(Pointer, Reason, I);

    if (const auto EscReason = EAI->getExtObjStatusIPA(Pointee.Obj);
        EscReason.any())
      addEscObjOrReason(Pointer, EscReason, I);
  }
}

void EscapeAnalysisInfo::EscapeState::forEachPointeeDo(
    const ObjAndPath &OAP,
    const std::function<void(const ObjAndPath &)> &Action) const {
  LLVM_DEBUG(dbgs() << "\t\t\tforEachPointeeDo: " << *OAP.Obj
                    << dbgPathToStr(OAP.Path) << "\n";);
  if (const auto Pointees = PointsTo.getPointees(OAP); Pointees)
    for (const ObjAndPath &Pointee : Pointees.value())
      Action(Pointee);
}

/// Add the alias: Alias --> PointeeValue
void EscapeAnalysisInfo::EscapeState::addPointsTo(const UnderlObjTy &Pointer,
                                                  const UnderlObjTy &Pointee,
                                                  const EscapeAnalysisInfo *EAI,
                                                  const Instruction *I) {
  assert(Pointer.Obj->getType()->isPointerTy() && "Alias must be a pointer\n");

  if ((Pointer.Obj == Pointee.Obj) || (Pointee.Obj == nullptr) ||
      PointsTo.containsPointsToPair(Pointer, Pointee))
    return;

  LLVM_DEBUG(dbgs() << "\taddPointsTo: " << *Pointer.Obj
                    << dbgPathToStr(Pointer.Path) << " --> " << *Pointee.Obj
                    << dbgPathToStr(Pointee.Path) << "\n");

  PointsTo.addPointsToPair(Pointer, Pointee);

  // Considering transitivity: recursively add new alias to all existing aliases
  // of Pointee
  forEachPointeeDo(Pointee, [&](const ObjAndPath &Ptee) {
    // Ptee inherits path of Pointee
    addPointsTo(Pointer, {{Ptee.Obj, Pointee.Path}, false}, EAI, I);
  });

  if (Pointee.Loaded)
    return;

  checkAndUpdEscStatus(Pointer, Pointee, EAI, I);

  // If pointer value was loaded, we should check whether pointee object is
  // escaped or not
  if (Pointer.Loaded) {
    LLVM_DEBUG(dbgs() << "\t\t\tPointer loaded\n";);
    forEachPointeeDo(Pointer, [&](const ObjAndPath &Ptee) {
      checkAndUpdEscStatus(Ptee, Pointee, EAI, I);
    });
  }
}

//===----------------------------------------------------------------------===//
// PointsToRelTy
//===----------------------------------------------------------------------===//

/// Merge with other PointsToRel object (needed in basic data flow analysis)
void EscapeAnalysisInfo::PointsToRelTy::merge(const PointsToRelTy &Other) {
  for (const auto &[Pointer, PathToPointeeMap] : Other.PointsToMap)
    for (const auto &[Path, PointeeSet] : PathToPointeeMap)
      for (const ObjAndPath &Pointee : PointeeSet)
        addPointsToPair({Pointer, Path}, Pointee);
}

/// Add an points-to relation between two objects
void EscapeAnalysisInfo::PointsToRelTy::addPointsToPair(
    const ObjAndPath &Pointer, const ObjAndPath &Pointee) {
  if (Pointer.Path == EmptyFieldPath)
    PointsToMap[Pointer.Obj][EmptyFieldPath].insert(Pointee);
  else
    PointsToMap[Pointer.Obj][Pointer.Path].insert(Pointee);
}

/// Check whether points-to relation contains Pointer-Pointee pair
bool EscapeAnalysisInfo::PointsToRelTy::containsPointsToPair(
    const ObjAndPath &Pointer, const ObjAndPath &Pointee) {

  const auto It = PointsToMap.find(Pointer.Obj);
  if (It == PointsToMap.end())
    return false;

  decltype(It->second)::const_iterator It2;
  if (Pointer.Path == EmptyFieldPath)
    It2 = It->second.find(EmptyFieldPath);
  else
    It2 = It->second.find(Pointer.Path);

  if (It2 == It->second.end())
    return false;

  return It2->second.contains(Pointee);
}

/// Get list of pointees for the object
/// Whether one field path covers the other: A is a prefix of B or B of A. The
/// empty path is the whole object and covers every field; an escaped field
/// covers the whole object, since an access to the whole touches that field.
/// Only incomparable paths -- two different fields -- are independent, and that
/// is the field-sensitivity the analysis claims.
static bool pathsOverlap(const FieldPathTy &A,
                         const FieldPathTy &B) {
  const size_t N = std::min(A.size(), B.size());
  return std::equal(A.begin(), A.begin() + N, B.begin());
}


std::optional<EscapeAnalysisInfo::PointsToRelTy::PointeeListTy>
EscapeAnalysisInfo::PointsToRelTy::getPointees(
    const ObjAndPath &Pointer) const {
  const auto It = PointsToMap.find(Pointer.Obj);
  if (It == PointsToMap.end())
    return std::nullopt;

  // Helper function to collect all pointees from paths
  auto collectAllPointees = [&](const PathToPointeeMap &PathToPointee)
      -> std::optional<PointeeListTy> {
    PointeeListTy AllPointees;
    for (const auto &[Path, Pointees] : PathToPointee)
      AllPointees.insert(Pointees.begin(), Pointees.end());
    return AllPointees.empty() ? std::nullopt : std::make_optional(AllPointees);
  };

  // Pointees recorded under every path that overlaps the query's: the whole
  // object's pointees cover every field, a field's cover the whole object,
  // and two different fields are independent -- the same lattice the escaped
  // set uses.
  const auto &PathToPointee = It->second;
  PointeeListTy Result;
  for (const auto &[Path, Pointees] : PathToPointee)
    if (pathsOverlap(Path, Pointer.Path))
      Result.insert(Pointees.begin(), Pointees.end());
  (void)collectAllPointees;
  return Result.empty() ? std::nullopt : std::make_optional(Result);
}

/// We need it to check if something changed in the data-flow analysis
bool EscapeAnalysisInfo::PointsToRelTy::operator==(
    const PointsToRelTy &Other) const {
  if (this == &Other)
    return true;
  return PointsToMap == Other.PointsToMap;
}

/// Print alias relation
void EscapeAnalysisInfo::PointsToRelTy::print(raw_ostream &OS) const {
  OS << "Alias relations:\n";
  for (const auto &[PointerObj, PathToPointee] : PointsToMap) {
    for (const auto &[Path, Pointees] : PathToPointee) {
      OS << "\tAlias: " << *PointerObj << dbgPathToStr(Path) << "\n";
      for (const auto &[PointeeObj, Path] : Pointees)
        OS << "\t\t\t --> " << *PointeeObj << dbgPathToStr(Path) << "\n";
    }
  }
  OS << "\n";
}

//===----------------------------------------------------------------------===//
// EscapeState
//===----------------------------------------------------------------------===//

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::EscapeState::getEscReason(const ObjAndPath &OAP) const {
  // Every recorded escape of this object whose path overlaps the query. The
  // lookup used to be an exact match on (object, path): an object escaped as
  // a whole -- {s,[]}, e.g. passed to a call -- did not cover a later access
  // to one of its fields, {s,[1]}, and the field write was elided.
  EscReasonTy Reason;
  for (auto It = EscapedObjs.lower_bound({OAP.Obj, EmptyFieldPath});
       It != EscapedObjs.end() && It->first.Obj == OAP.Obj; ++It)
    if (pathsOverlap(It->first.Path, OAP.Path))
      Reason |= It->second;
  return Reason;
}

bool EscapeAnalysisInfo::EscapeState::operator==(const EscapeState &ES) const {
  if (this == &ES)
    return true;
  return ((PointsTo == ES.PointsTo) && (EscapedObjs == ES.EscapedObjs));
}

void EscapeAnalysisInfo::EscapeState::addEscObjOrReason(const ObjAndPath &OAP,
                                                        EscReasonTy EscReason,
                                                        const Instruction *I) {
  LLVM_DEBUG(dbgs() << "\t\t\t\taddEscapeObjOrReason: " << dbgObjToStr(OAP.Obj)
                    << dbgPathToStr(OAP.Path) << "\n\t\t\t\tNew EscReason: ";
             printEscReason(EscReason););

  if (const auto EscObjIt = EscapedObjs.find(OAP);
      EscObjIt != EscapedObjs.end()) {
    // Object is already escaped - add the escape reason
    EscObjIt->second |= EscReason;
  } else {
    // If object escapes by empty path, then it escapes by all paths
    // 1. If the object has already escaped by EmptyFieldPath, do nothing
    if (EscapedObjs.find({OAP.Obj, EmptyFieldPath}) != EscapedObjs.end())
      return;

    // 2. If EmptyFieldPath is the new (current) path, then leave only it
    if (OAP.Path == EmptyFieldPath)
      for (auto It = EscapedObjs.begin(); It != EscapedObjs.end();) {
        if (It->first.Obj == OAP.Obj)
          It = EscapedObjs.erase(It);
        else
          ++It;
      }
    // Object has not escaped before - add it
    EscapedObjs.insert({OAP, EscReason});

    DEBUG_WITH_TYPE(PRINT_ESCAPING_CALLEES,
      if (I && (EscReason == PASSING_TO_CALL)) {
        // Print callee function, possible leads to escaping passed arguments
        if (const auto *CB = dyn_cast<CallBase>(I)) {
          if (CB->hasName(); const auto *Callee = CB->getCalledFunction()) {
            EscapeAnalysisGlobalInfo::EscFuncsFile
                << "Callee Function: " << Callee->getName().str() << "\n";

            if (const auto &DebugLoc = I->getDebugLoc())
              EscapeAnalysisGlobalInfo::EscFuncsFile
                  << "Source Line: " << DebugLoc.getLine() << "\n";
          }
        }
      });
  }
}

void EscapeAnalysisInfo::EscapeState::addEscObj(const ObjAndPath &EscObj,
                                                const EscReasonTy EscReason,
                                                const Instruction *I) {
  LLVM_DEBUG(dbgs() << "\t\taddEscObj: " << dbgObjToStr(EscObj.Obj) << "\n");
  SmallVector<ObjAndPath> WorkList;
  SmallSet<ObjAndPath, 4> Visited;

  WorkList.push_back(EscObj);

  while (!WorkList.empty()) {
    const ObjAndPath Curr = WorkList.pop_back_val();

    if (!Visited.insert(Curr).second)
      continue;

    addEscObjOrReason(Curr, EscReason, I);

    if (const auto Pointees = PointsTo.getPointees(Curr); Pointees.has_value())
      for (const auto &OAP : Pointees.value())
        if (!Visited.contains(OAP))
          WorkList.push_back(OAP);
  }
}

void EscapeAnalysisInfo::EscapeState::mergeEscapedObjects(
    const EscapeState &OtherES) {
  // Here we don't need to look through aliases, because if some object
  // has been added to EscapedObjects, then all it's aliases
  // have been added too
  for (const auto &[Obj, EscReason] : OtherES.EscapedObjs) {
    if (auto It = EscapedObjs.find(Obj); It != EscapedObjs.end())
      // Object exists, merge EscReason masks
      It->second |= EscReason;
    else
      // Object not exists, add it to the list with a reason
      EscapedObjs.insert({Obj, EscReason});
  }
}

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::getArgEscBottomTopIPA(const unsigned ArgNo,
                                          const Function *Func) const {
  if (const auto It = IPABottomTopInfo->find(Func);
      It != IPABottomTopInfo->end()) {
    return It->second.ArgEscapes[ArgNo] &
           EscReasonTy(~EscReasonBits::PTR_ARG_ALIASING);
  }
  return EscReasonBits::PTR_ARG_ALIASING;
}

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::getArgEscTopDownIPA(unsigned ArgNo,
                                        const Function *Func) const {
  if (const auto It = IPATopDownInfo->find(Func); It != IPATopDownInfo->end()) {
    if (It->second[ArgNo])
      return EscReasonBits::PTR_ARG_ALIASING;
    return 0;
  }
  return EscReasonBits::PTR_ARG_ALIASING;
}

bool EscapeAnalysisInfo::isNonConstGV(const Value *V) {
  if (const auto *GV = dyn_cast<GlobalVariable>(V))
    return !GV->isConstant();
  return false;
}

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::getExtObjStatus(const Value *V) {
  if (const auto *CI = dyn_cast<CallBase>(V);
      CI && CI->getFunctionType()->getReturnType()->isPointerTy()) {
    return EscReasonBits::ESCAPED_CALL;
  }

  if (isNonConstGV(V))
    return EscReasonBits::GPTR_ALIASING;

  if (isPointerArgument(V))
    return EscReasonBits::PTR_ARG_ALIASING;

  return 0;
}

static bool getIPAFuncRetEscStatus(
    std::shared_ptr<EscapeAnalysisInfo::IPABottomTopMap> IPABottomTopEscInfo,
    const Function *F) {
  if (const auto IPAEscInfoIt = IPABottomTopEscInfo->find(F);
      IPAEscInfoIt != IPABottomTopEscInfo->end()) {
    LLVM_DEBUG(dbgs() << "\tCall " << F->getName() << " returns escaped "
                      << IPAEscInfoIt->second.IsRetEscape << "\n";);
    return IPAEscInfoIt->second.IsRetEscape;
  }
  return false;
}

/// True when \p F is a library function TargetLibraryInfo knows, by
/// prototype and as available on the target, whose return value is fresh
/// memory. A name alone proves nothing: a program's own "malloc" with a
/// different signature is not the allocator.
static bool isCallNotReturnEscaped(const Function &F,
                                   const TargetLibraryInfo &TLI) {
  LibFunc Func;
  return TLI.getLibFunc(F, Func) && TLI.has(Func) &&
         !TLI.isReturnValueEscaping(Func);
}

static bool isSafeExternalCall(
    StringRef FuncName, const TargetLibraryInfo &TLI, unsigned ArgIndex,
    std::shared_ptr<EscapeAnalysisInfo::NonEscapingFuncsMap> NonEscapingFuncs =
        nullptr) {
  // Without a summary nothing external is known to be safe; the question
  // is about an argument, and a return-value predicate is no answer to it.
  if (!NonEscapingFuncs)
    return false;

  const auto FuncIt = NonEscapingFuncs->find(std::string(FuncName));
  if (FuncIt == NonEscapingFuncs->end())
    return false;

  const auto &ArgEscStatus = FuncIt->second;
  // A summary line with no argument list says nothing about any argument.
  if (ArgEscStatus.empty())
    return false;

  return ArgEscStatus.contains(ArgIndex); // listed = known not to escape
}

/// Check if it's a function call which can escape
static bool isCallMayEscape(
    const Value *V, const TargetLibraryInfo &TLI,
    std::shared_ptr<EscapeAnalysisInfo::IPABottomTopMap> IPABottomTopInfo =
        nullptr,
    std::shared_ptr<EscapeAnalysisInfo::NonEscapingFuncsMap> NonEscapingFuncs =
        nullptr) {
  const auto *CB = dyn_cast<CallBase>(V);
  if (!CB)
    return false;
  LLVM_DEBUG(dbgs() << "\t\tisCallMayEscape: " << *V << "\n";);
  if (isa<MemIntrinsic>(V))
    return false; // Intrinsics do not escape.

  // Check if the call is to a known memory allocation function.
  if (const Function *F = CB->getCalledFunction()) {
    if (F->isDeclaration()) {
      if (isCallNotReturnEscaped(*F, TLI))
        return false; // Memory allocation functions do not escape.
      return true;    // Unknown external function.
    }

    // Check if this function returns escaped value
    if (IPABottomTopInfo)
      return getIPAFuncRetEscStatus(IPABottomTopInfo, F);
  }

  return true;
}

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::getExtObjStatusIPA(const Value *V) const {
  const auto EscReason = getExtObjStatus(V);
  if (IPATopDownInfo && (EscReason == EscReasonBits::PTR_ARG_ALIASING)) {
    const auto *Arg = cast<Argument>(V);
    const auto EscReason =
        getArgEscTopDownIPA(Arg->getArgNo(), Arg->getParent());
    return EscReason;
  }

  if (IPABottomTopInfo && (EscReason == EscReasonBits::ESCAPED_CALL)) {
    if (isCallMayEscape(V, TLI, IPABottomTopInfo))
      return EscReasonBits::ESCAPED_CALL;
    return EscReasonBits::NO_ESCAPE;
  }

  return EscReason;
}

//===----------------------------------------------------------------------===//
// Main analysis
//===----------------------------------------------------------------------===//

EscapeAnalysisInfo::EscapeAnalysisInfo(
    const Function &Fn, const TargetLibraryInfo &TLI_,
    std::shared_ptr<NonEscapingFuncsMap> NonEscapingFuncs_,
    std::shared_ptr<IPABottomTopMap> IPABottomTopInfo_,
    std::shared_ptr<IPAArgEscFromCallsMap> IPAArgEscFromCallers_)
    : AnalyzedFunc(Fn), IPABottomTopInfo(IPABottomTopInfo_),
      IPATopDownInfo(IPAArgEscFromCallers_),
      NonEscapingFuncs(NonEscapingFuncs_), TLI(TLI_) {
  UnknownObj = UndefValue::get(PointerType::getUnqual(Fn.getContext()));
  LLVM_DEBUG(dbgs() << "\n|||||||||||||||||||||||||||||||||||||||||||||||||||||"
                       "|||||||||||||||||\n|||||||||||||||||||| Func "
                    << Fn.getName() << "\t||||||||||||||||||||||\n"
                       "|||||||||||||||||||||||||||||||||||||||||||||||||||||||"
                       "|||||||||||||||\n");
  std::deque<const BasicBlock *> WorkList;

  // Traverse CFG in reverse post-order
  ReversePostOrderTraversal<const Function *> RPOT(&AnalyzedFunc);
  FuncEscapeStates.clear(); // the union summarises these; rebuilt on demand
  for (const BasicBlock *BB : RPOT) {
    WorkList.push_back(BB);
    BBEscapeStates[BB] = EscapeState();
  }

  while (!WorkList.empty()) {
    const BasicBlock *BB = WorkList.front();
    WorkList.pop_front();

    LLVM_DEBUG(dbgs() << "****************** BB " << BB->getName() << " (func "
                      << Fn.getName() << ") ******************\n");

    EscapeState NewES = mergePredEscapeStates(BB);
    compBBEscapeState(BB, NewES);

    // If something changed, proceed with this BB
    LLVM_DEBUG(dbgs() << "\n>> Check changes for " << BB->getName() << " -- ");
    if (NewES != BBEscapeStates[BB]) {
      LLVM_DEBUG(dbgs() << "Changed!\n\n");
      // Add BB's successors to WorkList and update BB state
      for (const auto *SuccBB : successors(BB)) {
        // LLVM_DEBUG(dbgs() << "Add succ BB " << SuccBB->getName() << "\n");
        WorkList.push_back(SuccBB);
      }
      BBEscapeStates[BB] = NewES;
      // The per-function union is stale the moment a block state changes;
      // the analysis itself asks isEscapedForBBIPA mid-fixpoint, and under
      // -tsan-ea-flow-insensitive that answer comes from the union.
      FuncEscapeStates.clear();
    } else {
      LLVM_DEBUG(dbgs() << "Not Changed!\n\n");
    }

    LLVM_DEBUG(printEscapingForBB(BB, dbgs());
               BBEscapeStates[BB].getPointsTo().print(dbgs());
               dbgs() << "******** END of BB " << BB->getName()
                      << " ******** \n\n";);
  }
}

void EscapeAnalysisInfo::updRetEscStatus(
    const EscapeState &ES, const BasicBlock *BB,
    const SmallVectorImpl<UnderlObjTy> &UnderlObjs) {
  for (const auto &EO : UnderlObjs) {
    LLVM_DEBUG(dbgs() << "\t\treturn: check: " << *EO.Obj << "\n";);
    if (isEscapedForBBIPA(BB, EO) || ES.getEscReason(EO).any()) {
      LLVM_DEBUG(dbgs() << "\t\t\t\treturn is escaped\n");
      IsRetEscape = true;
      return;
    }
    LLVM_DEBUG(dbgs() << "\t\t\t\treturn is not escaped\n");
  }

  for (const auto &UO : UnderlObjs) {
    if (UO.Loaded) {
      LLVM_DEBUG(dbgs() << "\t\tupdRetEscStatus " << *UO.Obj << " Loaded\n");
      unsigned NumPointees = 0;
      ES.forEachPointeeDo(UO, [&](const ObjAndPath &Pointee) {
        ++NumPointees;
        if (isEscapedForBBIPA(BB, Pointee))
          IsRetEscape = true;
      });
      if (NumPointees == 0) // returned a pointer the analysis never saw stored
        IsRetEscape = true;
      if (IsRetEscape)
        return;
    }
  }
}

void EscapeAnalysisInfo::addEscapedPtrArgs(EscapeState &ES) const {
  // Iterate over the arguments of the function
  for (const Argument &Arg : AnalyzedFunc.args()) {
    if (!Arg.getType()->isPointerTy())
      continue;

    // Check if the argument is escaped using getExtObjStatusWithIPA
    const auto ArgEscReason = getExtObjStatusIPA(&Arg);
    if (ArgEscReason.any()) {
      LLVM_DEBUG(dbgs() << "Argument " << Arg.getName() << " is escaped: ";
                 printEscReason(ArgEscReason););
      // FIXME double check
      ES.addEscObj({&Arg, EmptyFieldPath}, ArgEscReason, nullptr);
    }
  }
}

/// Compute the resulting escape state for BB
void EscapeAnalysisInfo::compBBEscapeState(const BasicBlock *BB,
                                           EscapeState &ES) {
  if (BB->isEntryBlock() && IPABottomTopInfo)
    addEscapedPtrArgs(ES);

  for (const Instruction &I : *BB) {
    LLVM_DEBUG(dbgs() << "\n\nINSTR " << I << "\n");
    for (const Use &Opnd : I.operands()) {
      // A value that carries no pointer names no object. Every operand used
      // to be classified and walked -- doubles, integers, the value of a
      // volatile store or an atomic RMW -- and a walk that could not resolve
      // one was silently dropped; once such an operand meant "anything",
      // 26,000 of them per sqlite3.c meant everything. Integer laundering of
      // a pointer is handled where the pointer becomes an integer.
      if (!typeContainsPointerType(Opnd->getType()))
        continue;
      const auto [EscKind, EscDetails] = getEscInfoForOpnd(Opnd);

      if (EscKind == EscKindTy::NO_ESCAPE)
        continue;

      LLVM_DEBUG(dbgs() << "\nOPND: " << dbgObjToStr(Opnd););
      assert(EscDetails.has_value() && "EscDetails must be set");

      bool IsComplete = true;
      auto UnderlObjs = getUnderlyingMayEscObjs(
          Opnd.get(), TLI, MaxUnderlObjLookup, IPABottomTopInfo, &IsComplete);

      if (!IsComplete)
        ++NumEAUnknownOperands;
      if (!IsComplete && ClEAUnknownTop) {
        // The operand may point to anything the walk could not name. Stored,
        // passed or returned, it may hand any local to another thread; as an
        // alias target, anything may be read through it. UnknownObj stands
        // for that; the queries treat a state holding it as "everything
        // escaped". (This used to `continue`, dropping the verdict.)
        UnderlObjs.clear();
        UnderlObjs.push_back({{UnknownObj, EmptyFieldPath}, false});
        if (I.getOpcode() == Instruction::Ret)
          IsRetEscape = true;
      } else if (UnderlObjs.empty()) {
        continue;
      }
      // Returning an aggregate that carries a pointer returns that pointer;
      // the insertvalue that built it already escaped the object, this sets
      // the summary bit the callers read.
      if (I.getOpcode() == Instruction::Ret && !I.getOperand(0)->getType()->isPointerTy() &&
          typeContainsPointerType(I.getOperand(0)->getType()))
        IsRetEscape = true;

      if (EscKind == EscKindTy::MAY_ESCAPE) {
        LLVM_DEBUG(dbgs() << "\t-- MAY_ESCAPE --\n");
        const auto EscReason = std::get<EscReasonTy>(EscDetails.value());

        // Yes, looks ugly, but calls returning pointer is the only case when
        // register escapes, not a memory
        if (EscReason == EscReasonBits::ESCAPED_CALL) {
          UnderlObjs.clear();
          UnderlObjs.push_back({{&I, EmptyFieldPath}, false}); // INFO: Path
        }

        // If that's return instruction, we should check if it can return
        // a pointer to some external object
        if ((I.getOpcode() == Instruction::Ret) && (!IsRetEscape))
          updRetEscStatus(ES, BB, UnderlObjs);

        for (const auto &UO : UnderlObjs) {
          ES.addEscObj(UO, EscReason, &I);
          if (UO.Loaded) {
            LLVM_DEBUG(dbgs() << "\t\tLoaded\n");
            ES.forEachPointeeDo(UO, [&](const ObjAndPath &Pointee) {
              ES.addEscObj(Pointee, EscReason, &I);
            });
            continue;
          }

          const auto ExtEscReason = getExtObjStatus(UO.Obj);
          LLVM_DEBUG(dbgs() << "\t\tEscObj: " << dbgObjToStr(UO.Obj)
                            << "\n\t\tExtEscReason: ";
                     printEscReason(ExtEscReason););
          if (ExtEscReason != EscReasonBits::GPTR_ALIASING)
            ES.addEscObj(UO, EscReason, &I);
        }
      } else {
        assert(EscKind == EscKindTy::MAY_ALIASING);
        const auto AliasList =
            std::get<SmallVector<UnderlObjTy>>(EscDetails.value());

        LLVM_DEBUG(dbgs() << "\t-- ALIASING --\n";
                   dbgPrintAliasCand(AliasList););

        for (const auto &Alias : AliasList)
          for (const auto &Pointee : UnderlObjs)
            ES.addPointsTo(Alias, Pointee, this, &I);
      }
    }
  }
}

EscapeAnalysisInfo::EscapeState
EscapeAnalysisInfo::mergePredEscapeStates(const BasicBlock *BB) {
  EscapeState MergedES;
  // Merge states of predecessors
  for (auto *PredBB : predecessors(BB)) {
    // LLVM_DEBUG(dbgs() << "Merge to << " << BB->getName() << " <-- "
    // << PredBB->getName() << "\n");
    const auto &PredES = BBEscapeStates[PredBB];
    MergedES.merge(PredES, this);
  }
  return MergedES;
}

//===----------------------------------------------------------------------===//
// Get escape info functions
//===----------------------------------------------------------------------===//

/// Check whether type contains pointers
bool EscapeAnalysisInfo::typeContainsPointerType(const Type *Ty) {
  if (Ty->isPointerTy())
    return true;
  // Recurse into every aggregate, not only structs: an array or vector of
  // pointers carries them just as a struct field does.
  if (!Ty->isStructTy() && !Ty->isArrayTy() && !Ty->isVectorTy())
    return false;

  for (const Type *EltTy : Ty->subtypes())
    if (typeContainsPointerType(EltTy))
      return true;
  return false;
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoCall(const Use &U, const Instruction *I) const {
  // LLVM_DEBUG(dbgs() << " -- Call/Invoke\n");
  const auto *Call = cast<CallBase>(I);

  // Not captured if the callee is readonly, doesn't return a copy through
  // its return value and doesn't unwind (a readonly function can leak bits
  // by throwing an exception or not depending on the input value).
  if (Call->onlyReadsMemory() && Call->doesNotThrow() &&
      Call->getType()->isVoidTy())
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  if (isCallMayEscape(Call, TLI, IPABottomTopInfo)) {
    const auto *F = dyn_cast<Function>(U.get());
    if (F && (F != Call->getCalledFunction())) {
      if (IPABottomTopInfo)
        (*IPABottomTopInfo)[F].IsPassedAsPtr = true;
    }

    if (Call->getType()->isPointerTy())
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::ESCAPED_CALL};
  }

  // The pointer is not captured if returned pointer is not captured.
  // NOTE: CaptureTracking users should not assume that only functions
  // marked with nocapture do not capture. This means that places like
  // getUnderlyingObject in ValueTracking or DecomposeGEPExpression
  // in BasicAA also need to know about this property.
  if (isIntrinsicReturningPointerAliasingArgumentWithoutCapturing(Call, true))
    return {EscKindTy::MAY_ALIASING, getUnderlyingMayEscObjs(I, TLI)};

  if (const auto *MI = dyn_cast<MemIntrinsic>(Call)) {
    // Volatile operations effectively capture the memory location that they
    // load and store to.
    if (MI->isVolatile())
      return {EscKindTy::MAY_ESCAPE, EscReasonTy(EscReasonBits::VOLATILE)};

    const auto *Src = MI->getArgOperand(1);
    const auto *Dst = MI->getArgOperand(0);

    // A transfer copies the source's bytes into the destination. If those
    // bytes include a pointer into a local, that pointer now also lives at the
    // destination, so the local escapes with it. This covers memmove as well
    // as memcpy (both are MemTransferInst), and any type that contains a
    // pointer -- an array of pointers as much as a struct. Previously only
    // memcpy of a struct was modelled, so a pointer copied by memmove, or one
    // living in a pointer array, slipped through and its target was reported
    // local.
    if (isa<MemTransferInst>(MI) && (Src == U.get()))
      if (const auto *Alloca = dyn_cast<AllocaInst>(U.get()))
        if (const Type *SrcTy = Alloca->getAllocatedType();
            SrcTy && typeContainsPointerType(SrcTy))
          return {EscKindTy::MAY_ALIASING, getUnderlyingMayEscObjs(Dst, TLI)};
  }

  // Calling a function pointer does not in itself cause the pointer to
  // be captured.  This is a subtle point considering that (for example)
  // the callee might return its own address.  It is analogous to saying
  // that loading a value from a pointer does not cause the pointer to be
  // captured, even though the loaded value might be the pointer itself
  // (think of self-referential objects).
  if (Call->isCallee(&U) || (!Call->isDataOperand(&U)))
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  if (const Function *CalledFunc = Call->getCalledFunction();
      CalledFunc && Call->isArgOperand(&U)) {
    const unsigned ArgIndex = Call->getArgOperandNo(&U);
    // Some functions can be taken as safe external calls
    LibFunc Func;
    if (TLI.getLibFunc(*CalledFunc, Func) && TLI.has(Func) &&
        !TLI.doesArgEscape(Func, ArgIndex))
      return {EscKindTy::NO_ESCAPE, std::nullopt};

    if (isSafeExternalCall(CalledFunc->getName(), TLI, ArgIndex,
                           NonEscapingFuncs))
      return {EscKindTy::NO_ESCAPE, std::nullopt};
  }

  // Check if that's the argument which can escape through this call
  // Not captured if only passed via 'nocapture' arguments.
  if (!Call->doesNotCapture(Call->getDataOperandNo(&U)) &&
      U->getType()->isPointerTy()) {
    // If that's IPA, need to check whether passing to calls is escape or not
    if (!IPABottomTopInfo)
      // If not IPA, each call is the escape for each pointer-typed argument
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::PASSING_TO_CALL};

    const Function *Callee = Call->getCalledFunction();
    const auto ArgNo = Call->getDataOperandNo(&U);
    if (!isLocalAndExactFunc(Callee) ||
        // If it's variadic function, arguments after fixed ones are escaped
        (Callee->isVarArg() &&
         (ArgNo >= Callee->getFunctionType()->getNumParams())))
      // If IPA, then argument escapes only in a Call of non-local function
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::PASSING_TO_CALL};

    // If called function is local, find argument information in ArgsEscapes
    // provided by IPA callgraph traversal
    const auto ArgEscReason = getArgEscBottomTopIPA(ArgNo, Callee);
    if (ArgEscReason.any() && (ArgEscReason != EscReasonBits::PTR_ARG_ALIASING))
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::PASSING_TO_CALL};
  }
  return {EscKindTy::NO_ESCAPE, std::nullopt};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoLoad(const Instruction *I) const {
  // LLVM_DEBUG(dbgs() << " -- Load\n");
  // Volatile loads make the address observable.
  if (cast<LoadInst>(I)->isVolatile())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::VOLATILE};
  return {EscKindTy::NO_ESCAPE, std::nullopt};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoStore(const Use &U, const Instruction *I) const {
  // LLVM_DEBUG(dbgs() << " -- Store\n");
  // Volatile stores make the address observable.
  if (cast<StoreInst>(I)->isVolatile())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::VOLATILE};

  if (U.getOperandNo() != 0)
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  // If the stored type is not a pointer, it's not an escape neither aliasing
  if (!cast<StoreInst>(I)->getValueOperand()->getType()->isPointerTy())
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  // dbgs() << *cast<StoreInst>(I)->getPointerOperandType() << "\n";

  bool DstComplete = true;
  const auto DstObjs = getUnderlyingMayEscObjs(
      I->getOperand(1), TLI, MaxUnderlObjLookup, nullptr, &DstComplete);
  // A destination the walk cannot name may be shared memory: the stored
  // pointer escapes. (An empty list used to mean "no escape".)
  if (!DstComplete || DstObjs.empty())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::OTHER};

  return {EscKindTy::MAY_ALIASING, DstObjs};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoAtomicRMW(const Use &U,
                                        const Instruction *I) const {
  LLVM_DEBUG(dbgs() << " -- AtomicRMW\n");
  // atomicrmw conceptually includes both a load and store from
  // the same location.
  // As with a store, the location being accessed is not captured,
  // but the value being stored is.
  // Volatile stores make the address observable.
  const auto *ARMWI = cast<AtomicRMWInst>(I);
  if (U.getOperandNo() == 1 || ARMWI->isVolatile())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::VOLATILE};
  return {EscKindTy::NO_ESCAPE, std::nullopt};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoAtomicCmpXchg(const Use &U,
                                            const Instruction *I) const {
  LLVM_DEBUG(dbgs() << " -- AtomicCmpXchg\n");
  // cmpxchg conceptually includes both a load and store from
  // the same location.
  // As with a store, the location being accessed is not captured,
  // but the value being stored is.
  // Volatile stores make the address observable.
  if (const auto *ACXI = cast<AtomicCmpXchgInst>(I);
      U.getOperandNo() == 1 || U.getOperandNo() == 2 || ACXI->isVolatile())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::OTHER};
  return {EscKindTy::NO_ESCAPE, std::nullopt};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoGetElementPtr(const Instruction *I) const {
  // LLVM_DEBUG(dbgs() << " -- GetElementPtr\n");
  // AA does not support pointers of vectors, so GEP vector splats need to
  // be considered as captures.
  if (I->getType()->isVectorTy())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::OTHER};

  // GEP itself is not escape or alias
  return {EscKindTy::NO_ESCAPE, std::nullopt};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoICmp(const Use &U, const Instruction *I) const {
  // LLVM_DEBUG(dbgs() << " -- ICmp\n");
  const unsigned Idx = U.getOperandNo();
  const unsigned OtherIdx = 1 - Idx;
  if (const auto *CPN =
          dyn_cast<ConstantPointerNull>(I->getOperand(OtherIdx))) {
    // Don't count comparisons of a no-alias return value against null as
    // captures. This allows us to ignore comparisons of malloc results
    // with null, for example.
    if (CPN->getType()->getAddressSpace() == 0)
      if (isNoAliasCall(U.get()->stripPointerCasts()))
        return {EscKindTy::NO_ESCAPE, std::nullopt};

    if (!I->getFunction()->nullPointerIsDefined()) {
      const auto *O = I->getOperand(Idx)->stripPointerCastsSameRepresentation();
      // Comparing a dereferenceable_or_null pointer against null cannot
      // lead to pointer escapes, because if it is not null it must be a
      // valid (in-bounds) pointer.
      if (isDereferenceableOrNull(O, I->getModule()->getDataLayout()))
        return {EscKindTy::NO_ESCAPE, std::nullopt};
    }
  }

  // Otherwise, be conservative. There are crazy ways to capture pointers
  // using comparisons.
  // return {CaptureKind::MAY_CAPTURE, std::nullopt};
  return {EscKindTy::NO_ESCAPE, std::nullopt};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoRet(const Use &U) const {
  // LLVM_DEBUG(dbgs() << " -- Ret\n");
  // If not return pointer, means that's not escape
  // Returning null pointer is not escape
  if (!U->getType()->isPointerTy() || (isa<ConstantPointerNull>(U.get())))
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  return {EscKindTy::MAY_ESCAPE, EscReasonBits::RET_PTR};
}

/// Where does the integer made from a pointer go? Arithmetic, comparisons
/// and a conversion back to a pointer keep it in sight: the pointer the
/// inttoptr yields is followed by the object walk from wherever it is used,
/// or fails closed if the arithmetic is beyond the walk. Anything else -- a
/// store of the integer, a call, a return, a truncation -- takes the address
/// out of sight and is an escape. Bounded, so a long chain is an escape too.
static bool ptrIntEscapes(const Value *Int, unsigned Depth = 0) {
  if (Depth > 8)
    return true;
  for (const User *U : Int->users()) {
    const auto *UI = dyn_cast<Instruction>(U);
    if (!UI)
      return true;
    switch (UI->getOpcode()) {
    case Instruction::IntToPtr:
    case Instruction::ICmp:
      continue;
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::Mul:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::PHI:
    case Instruction::Select:
      if (ptrIntEscapes(UI, Depth + 1))
        return true;
      continue;
    default:
      return true;
    }
  }
  return false;
}

/// Determine what kind of escape behaviour V may exhibit, return
/// escape reason and list of aliases if applicable.
EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoForOpnd(const Use &U) const {
  const auto *I = dyn_cast<Instruction>(U.getUser());
  if (!I)
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  switch (I->getOpcode()) {
  case Instruction::Call:
  case Instruction::Invoke:
    return getEscInfoCall(U, I);
  case Instruction::Load:
    return getEscInfoLoad(I);
  case Instruction::Store:
    return getEscInfoStore(U, I);
  case Instruction::AtomicRMW:
    return getEscInfoAtomicRMW(U, I);
  case Instruction::AtomicCmpXchg:
    return getEscInfoAtomicCmpXchg(U, I);
  case Instruction::GetElementPtr:
    return getEscInfoGetElementPtr(I);
  case Instruction::ICmp:
    return getEscInfoICmp(U, I);
  case Instruction::Ret:
    return getEscInfoRet(U);
  case Instruction::CallBr:
    return getEscInfoCall(U, I);
  // Transparent: the pointer flows on as a value the object walk follows
  // from the access side; the use itself hands it to nobody.
  case Instruction::BitCast:
  case Instruction::AddrSpaceCast:
  case Instruction::PHI:
  case Instruction::Select:
  case Instruction::Freeze:
  case Instruction::Br:
  case Instruction::Switch:
    return {EscKindTy::NO_ESCAPE, std::nullopt};
  // The pointer leaves the walk's sight: as an integer, inside an aggregate
  // or vector, in an exception object, through varargs or an indirect branch.
  case Instruction::PtrToInt:
    if (ptrIntEscapes(I))
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::OTHER};
    return {EscKindTy::NO_ESCAPE, std::nullopt};
  case Instruction::InsertValue:
  case Instruction::InsertElement:
  case Instruction::ShuffleVector:
  case Instruction::Resume:
  case Instruction::VAArg:
  case Instruction::IndirectBr:
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::OTHER};
  default:
    // Anything not listed that carries a pointer is an escape. The old
    // default was "no escape", with a comment wondering whether that was
    // too aggressive; it was.
    if (typeContainsPointerType(U->getType()))
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::OTHER};
    return {EscKindTy::NO_ESCAPE, std::nullopt};
  }
}

bool EscapeAnalysisInfo::isDereferenceableOrNull(const Value *O,
                                                 const DataLayout &DL) {
  // We want comparisons to null pointers to not be considered capturing,
  // but need to guard against cases like gep(p, -ptrtoint(p2)) == null,
  // which are equivalent to p == p2 and would capture the pointer.
  //
  // A dereferenceable pointer is a case where this is known to be safe,
  // because the pointer resulting from such a construction would not be
  // dereferenceable.
  //
  // It is not sufficient to check for inbounds GEP here, because GEP with
  // zero offset is always inbounds.
  bool CanBeNull, CanBeFreed;
  return O->getPointerDereferenceableBytes(DL, CanBeNull, CanBeFreed);
}

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::findObjInBBEscState(const BasicBlock *BB,
                                        const ObjAndPath &OAP) const {
  const auto It = BBEscapeStates.find(BB);
  assert((It != BBEscapeStates.end()) && "Cannot find BBEscapeState for BB\n");
  return It->second.getEscReason(OAP);
}

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::findObjInFuncEscState(const Function *F,
                                          const ObjAndPath &OAP) const {
  auto It = FuncEscapeStates.find(F);
  if (It == FuncEscapeStates.end()) {
    EscapeState Union;
    for (const BasicBlock &BB : *F) {
      const auto BIt = BBEscapeStates.find(&BB);
      if (BIt == BBEscapeStates.end())
        continue;
      for (const auto &[Obj, Reason] : BIt->second.getEscObjs())
        Union.addEscObjOrReason(Obj, Reason, nullptr);
    }
    It = FuncEscapeStates.try_emplace(F, std::move(Union)).first;
  }
  return It->second.getEscReason(OAP);
}

bool EscapeAnalysisInfo::isEscapedForBBImpl(const BasicBlock *BB,
                                            const ObjAndPath &OAP,
                                            EscReasonTy *EscReason,
                                            bool UseIPA) const {
  const auto ExtStatus =
      UseIPA ? getExtObjStatusIPA(OAP.Obj) : getExtObjStatus(OAP.Obj);

  if (ExtStatus.any()) {
    if (EscReason)
      *EscReason = ExtStatus;
    return true;
  }

  // A state holding UnknownObj escaped is "everything escaped".
  const ObjAndPath Top{UnknownObj, EmptyFieldPath};
  auto FoundStatus = ClEAFlowInsensitive
                         ? findObjInFuncEscState(BB->getParent(), Top)
                         : findObjInBBEscState(BB, Top);
  if (!FoundStatus.any())
    FoundStatus = ClEAFlowInsensitive
                      ? findObjInFuncEscState(BB->getParent(), OAP)
                      : findObjInBBEscState(BB, OAP);
  if (FoundStatus.any()) {
    if (EscReason)
      *EscReason = FoundStatus;
    return true;
  }

  if (EscReason)
    *EscReason = 0;
  return false;
}

/// Is Value V is escaping in some path from Entry to BB?
bool EscapeAnalysisInfo::isEscapedForBB(const BasicBlock *BB,
                                        const ObjAndPath &OAP,
                                        EscReasonTy *EscReason) const {
  return isEscapedForBBImpl(BB, OAP, EscReason, false);
}

/// Is Value V is escaping in some path from Entry to BB?
bool EscapeAnalysisInfo::isEscapedForBBIPA(const BasicBlock *BB,
                                           const ObjAndPath &OAP,
                                           EscReasonTy *EscReason) const {
  return isEscapedForBBImpl(BB, OAP, EscReason, true);
}

bool EscapeAnalysisInfo::isEscapedInFuncIPA(const Function *F,
                                            const ObjAndPath &OAP,
                                            EscReasonTy *EscReason) const {
  const auto ExtStatus = getExtObjStatusIPA(OAP.Obj);
  if (ExtStatus.any()) {
    if (EscReason)
      *EscReason = ExtStatus;
    return true;
  }
  auto FoundStatus = findObjInFuncEscState(F, {UnknownObj, EmptyFieldPath});
  if (!FoundStatus.any())
    FoundStatus = findObjInFuncEscState(F, OAP);
  if (FoundStatus.any()) {
    if (EscReason)
      *EscReason = FoundStatus;
    return true;
  }
  if (EscReason)
    *EscReason = 0;
  return false;
}

/// Make action for each pointee, if given object points to something
void EscapeAnalysisInfo::forEachPointeeDo(
    const ObjAndPath &OAP, const BasicBlock *BB,
    const std::function<void(const ObjAndPath &)> &Action) const {
  const auto It = BBEscapeStates.find(BB);
  assert(It != BBEscapeStates.end());
  It->second.forEachPointeeDo(OAP, Action);
}

/// Return escape reason for V in BB
EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::getFullEscReasonForBB(const BasicBlock *BB,
                                          const ObjAndPath &OAP) const {
  return getExtObjStatusIPA(OAP.Obj) | findObjInBBEscState(BB, OAP);
}

/// Is Value V is escaping somewhere in the function
bool EscapeAnalysisInfo::isEscapedForFunc(const ObjAndPath &OAP,
                                          EscReasonTy *EscReason) const {
  EscReasonTy CombinedEscReason;
  for (const auto &BB : AnalyzedFunc) {
    if (pred_empty(&BB) && !BB.isEntryBlock())
      continue;
    CombinedEscReason |= getFullEscReasonForBB(&BB, OAP);
  }

  if (EscReason)
    *EscReason = CombinedEscReason;

  return CombinedEscReason.any();
}

void EscapeAnalysisInfo::printEscapingForBB(const BasicBlock *BB,
                                            raw_ostream &OS) const {
  const auto It = BBEscapeStates.find(BB);
  if ((It == BBEscapeStates.end()) || (It->second.getEscObjs().empty()))
    return;

  bool PrintedEscapeHeader = false;
  for (const auto &OAP : It->second.getEscObjs()) {
    if (!PrintedEscapeHeader) {
      OS << "Escaping objects for BB " << BB->getName() << ":\n";
      PrintedEscapeHeader = true;
    }

    if (!isa<GlobalValue>(OAP.first.Obj)) {
      // OS << *OAP.first.Obj << "\n";
      OS << *OAP.first.Obj << dbgPathToStr(OAP.first.Path) << "\n";
      // LLVM_DEBUG(OS << "EscReason: "; printEscReason(V.second); );
    }
  }
  if (PrintedEscapeHeader)
    OS << "\n";
}

void EscapeAnalysisInfo::print(raw_ostream &OS) const {
  for (const auto &BB : AnalyzedFunc)
    printEscapingForBB(&BB, OS);
  // LLVM_DEBUG(if (IPAFuncEscInfo)
  // OS << "IsRetEscape: " << IsRetEscape << "\n\n");
}

AnalysisKey EscapeAnalysis::Key;

EscapeAnalysis::Result EscapeAnalysis::run(Function &F,
                                           FunctionAnalysisManager &AM) {
  EscapeAnalysisInfo EAI(F, AM.getResult<TargetLibraryAnalysis>(F));
  return EAI;
}

PreservedAnalyses
EscapeAnalysisPrinterPass::run(Function &F, FunctionAnalysisManager &AM) const {
  OS << "Printing analysis 'Escape Analysis' for function '" << F.getName()
     << "':\n";
  AM.getResult<EscapeAnalysis>(F).print(OS);
  return PreservedAnalyses::all();
}

//===----------------------------------------------------------------------===//
// Escape analysis global (IPA)
//===----------------------------------------------------------------------===//

void EscapeAnalysisGlobalInfo::setAllPtrArgsNotEscaped(
    EscapeAnalysisInfo::IPABottomTopMap &IPAFuncEscInfo, const Function *F) {
  for (const auto &Arg : F->args())
    if (Arg.getType()->isPointerTy())
      IPAFuncEscInfo[F].ArgEscapes[Arg.getArgNo()] = false;
}

bool EscapeAnalysisGlobalInfo::isRecursiveCallGraphNode(
    const CallGraphNode *CGN) {
  const Function *F = CGN->getFunction();
  for (auto CI = CGN->begin(), CE = CGN->end(); CI != CE; ++CI) {
    CallGraphNode *Callee = CI->second;
    if (Callee && Callee->getFunction() == F)
      return true;
  }
  return false;
}

void EscapeAnalysisGlobalInfo::updIPAFuncEscInfo(
    const Function *F, const EscapeAnalysisInfo &EAI) const {
  LLVM_DEBUG(dbgs() << "Upd IPAFuncEscInfo for F '" << F->getName() << "'\n");
  for (const auto &Arg : F->args()) {
    EscapeAnalysisInfo::EscReasonTy ArgEscReason;
    LLVM_DEBUG(dbgs() << "@@@@@@@@@ ArgEsc: " << Arg << " -- Escape status: "
                      << EAI.isEscapedForFunc({&Arg, EmptyFieldPath}) << "\n";);

    // FIXME: double check if std::nullopt is correct here
    EAI.isEscapedForFunc({&Arg, EmptyFieldPath}, &ArgEscReason);

    if (IPABottomTopEscInfo->find(F) != IPABottomTopEscInfo->end()) {
      if ((*IPABottomTopEscInfo)[F].ArgEscapes.find(Arg.getArgNo()) !=
          (*IPABottomTopEscInfo)[F].ArgEscapes.end()) {
        LLVM_DEBUG(dbgs() << "Existing ArgEscape: ";
                   EscapeAnalysisInfo::printEscReason(
                       (*IPABottomTopEscInfo)[F].ArgEscapes[Arg.getArgNo()]);
                   dbgs() << "\n";);
      }
    }

    (*IPABottomTopEscInfo)[F].ArgEscapes[Arg.getArgNo()] = ArgEscReason;
  }
  LLVM_DEBUG(dbgs() << "@@@@@@@@@ RetEsc: " << EAI.getIsRetEscape() << "\n");
  (*IPABottomTopEscInfo)[F].IsRetEscape = EAI.getIsRetEscape();
}

bool EscapeAnalysisGlobalInfo::traverseSCCsAndInitIPAEscInfo(
    CallGraph &CG, SmallPtrSet<const Function *, 8> &RecursiveFuncs) {
  for (auto It = scc_begin(&CG); !It.isAtEnd(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;
    assert(!SCC.empty() && "SCC with no functions?");
    LLVM_DEBUG(printSCC(SCC));

    if (SCC.size() == 1) {
      // Check if it's recursive call or not
      const auto *F = SCC[0]->getFunction();
      if (!isLocalAndExactFunc(F))
        continue;

      if (isRecursiveCallGraphNode(SCC[0])) {
        setAllPtrArgsNotEscaped(*IPABottomTopEscInfo, F);
        (*IPABottomTopEscInfo)[F].IsRecursive = true;
        RecursiveFuncs.insert(F);
      }
    } else { // SCC.size() > 1
      for (const CallGraphNode *CGN : SCC) {
        const auto F = CGN->getFunction();
        assert(F && !F->isDeclaration());
        setAllPtrArgsNotEscaped(*IPABottomTopEscInfo, F);
      }
    }
  }
  return false;
}

DenseMap<const Function *, SmallVector<const CallBase *>>
EscapeAnalysisGlobalInfo::getFuncToCallSitesMap() {
  // Map: Function -> list of call instructions
  decltype(getFuncToCallSitesMap()) FuncToCallSitesMap;

  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;

    FuncToCallSitesMap[&F] = {};
    for (const Use &U : F.uses())
      if (const CallBase *CB = dyn_cast<CallBase>(U.getUser())) {
        const Function *CalledFunc = CB->getCalledFunction();
        if (CalledFunc && !CalledFunc->isDeclaration())
          FuncToCallSitesMap[CalledFunc].push_back(CB);
      }
  }

  return FuncToCallSitesMap;
}

void EscapeAnalysisGlobalInfo::evalTopDownArgEscStatus(
    const DenseMap<const Function *, SmallVector<const CallBase *>>
        &FuncCallSites,
    const Function *F) const {

  // If function has been passed as a pointer (e.g. to pthread_create)
  // then we treat all arguments as escaped
  assert(IPABottomTopEscInfo->count(F) &&
         "Entry for function F not found in IPABottomTopEscInfo");
  if ((*IPABottomTopEscInfo)[F].IsPassedAsPtr) {
    LLVM_DEBUG(dbgs() << "Function " << F->getName()
                      << " is passed as a pointer.\n");
    IPATopDownArgEscInfo->try_emplace(F, F->arg_size(), true);
    return;
  }
  // The direct call sites are the whole story only for a function nobody
  // else can reach: local linkage, and no use of its address at all -- a
  // pointer in a dispatch table's initializer, a store into a slot, or a
  // pass to a defined registrar that merely keeps it were not calls, and
  // the callers behind them pass whatever they please.
  if (F->hasAddressTaken() || (!TsanWholeProgram && !F->hasLocalLinkage())) {
    LLVM_DEBUG(dbgs() << "Function " << F->getName()
                      << " is reachable through its address.\n");
    IPATopDownArgEscInfo->try_emplace(F, F->arg_size(), true);
    return;
  }

  const auto It = IPATopDownArgEscInfo->try_emplace(F, F->arg_size(), false);
  auto &IsArgEscaped = It.first->second;

  LLVM_DEBUG({
    dbgs() << "\nIsArgEscaped for function " << F->getName() << ": [";
    for (size_t i = 0; i < IsArgEscaped.size(); ++i) {
      dbgs() << (IsArgEscaped[i] ? "true" : "false");
      if (i != IsArgEscaped.size() - 1)
        dbgs() << ", ";
    }
    dbgs() << "]\n";
  });

  const auto CallSitesIter = FuncCallSites.find(F);
  if (CallSitesIter == FuncCallSites.end())
    return;

  // Iterate through all instructions calling the function F
  for (const CallBase *CB : CallSitesIter->second) {
    // Check if arguments escape in this specific call
    LLVM_DEBUG(dbgs() << "\n\tCall: " << *CB << "\n");

    // Iterate over the parameters pass to this call instruction
    for (unsigned ArgIdx = 0; ArgIdx < CB->arg_size(); ++ArgIdx) {
      // Consider variable argument functions
      if (F->isVarArg() && (ArgIdx >= F->getFunctionType()->getNumParams()))
        break;

      const Value *Arg = CB->getArgOperand(ArgIdx);
      LLVM_DEBUG(dbgs() << "\n\tArg " << ArgIdx << ": " << *Arg << "\n");
      if (!isPointerArgument(F->getArg(ArgIdx)))
        continue;

      if (IsArgEscaped[ArgIdx])
        // Go to the next argument: this one is escaped in at least one
        // call, that's enough
        continue;

      if (isa<AllocaInst>(getUnderlyingObject(Arg)))
        if (!PointerMayBeCaptured(Arg, true, true)) {
          LLVM_DEBUG(dbgs() << "\t\tArg " << ArgIdx << " is NOT captured\n");
          continue;
        }

      auto *Func = const_cast<Function *>(CB->getFunction());
      const auto &TLI =
          MAM.getResult<FunctionAnalysisManagerModuleProxy>(M)
              .getManager()
              .getResult<TargetLibraryAnalysis>(*Func);
      const auto UnderlObjs =
          EscapeAnalysisInfo::getUnderlyingMayEscObjs(Arg, TLI);
      const auto EAIIt = FuncEscapeInfo.find(CB->getFunction());
      assert(EAIIt != FuncEscapeInfo.end());
      const EscapeAnalysisInfo &CallEAI = EAIIt->second;
      const auto *BB = CB->getParent();

      for (const auto &UnderlObj : UnderlObjs) {
        auto checkEscapeStatus = [&](const ObjAndPath &OAP) {
          IsArgEscaped[ArgIdx] |= CallEAI.isEscapedForBBIPA(BB, OAP);
          LLVM_DEBUG(dbgs() << "\t\tArg " << ArgIdx
                            << " (underl obj: " << *OAP.Obj << ") is escaped: "
                            << (IsArgEscaped[ArgIdx] ? "YES" : "NO") << "\n");
          return IsArgEscaped[ArgIdx];
        };

        if (UnderlObj.Loaded) {
          unsigned NumPointees = 0;
          CallEAI.forEachPointeeDo(UnderlObj, BB, [&](const ObjAndPath &OAP) {
            ++NumPointees;
            return checkEscapeStatus(OAP);
          });
          if (NumPointees == 0) // a pointer the caller never saw stored
            IsArgEscaped[ArgIdx] = true;
        } else {
          checkEscapeStatus(UnderlObj);
        }

        if (IsArgEscaped[ArgIdx])
          break;
      }
    }
  }
}

bool EscapeAnalysisGlobalInfo::traverseCGBottomTop(
    CallGraph &CG, const SmallPtrSetImpl<const Function *> &RecursiveFuncs,
    SmallVector<std::vector<CallGraphNode *>> &SCCList) {
  // Main callgraph traversal
  for (auto It = scc_begin(&CG); !It.isAtEnd(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;
    assert(!SCC.empty() && "SCC with no functions?");
    SCCList.push_back(SCC);

    bool Converged = false;
    while (!Converged) {
      Converged = true;
      for (const CallGraphNode *CGN : SCC) {
        Function *F = CGN->getFunction();
        if (!F || F->isDeclaration())
          continue;

        // Build escape summary for a function
        FuncEscapeInfo.erase(F);

        const auto &TLI = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M)
                              .getManager()
                              .getResult<TargetLibraryAnalysis>(*F);

        const auto [It, Inserted] = FuncEscapeInfo.try_emplace(
            F,
            EscapeAnalysisInfo(*F, TLI, NonEscapingFuncs, IPABottomTopEscInfo));

        const auto &EAI = It->second;
        // For non-recursive functions, no need to iterate until convergence
        if ((SCC.size() == 1) && !RecursiveFuncs.contains(F)) {
          updIPAFuncEscInfo(F, EAI);
          break;
        }

        const auto PrevIPAFuncInfo = (*IPABottomTopEscInfo)[F];
        updIPAFuncEscInfo(F, EAI);

        if (PrevIPAFuncInfo != (*IPABottomTopEscInfo)[F]) {
          Converged = false;
          LLVM_DEBUG(dbgs() << "++++++ Func " << F->getName()
                            << " -- NOT Converging ++++++\n\n";
                     It->second.print(dbgs()););
        } else {
          LLVM_DEBUG(dbgs() << "++++++ Func " << F->getName()
                            << " -- Converging ++++++\n\n";);
        }
      }
    }
  }
  return false;
}

bool EscapeAnalysisGlobalInfo::isFuncPassedToObjCSelector(
    const Function *F) const {
  for (const GlobalVariable &GV : M.globals()) {
    if (GV.getName().starts_with("OBJC_SELECTOR_REFERENCES_") &&
        GV.hasInitializer()) {
      const auto *Initializer = GV.getInitializer();

      if (Initializer->getName() == "OBJC_METH_VAR_NAME_") {
        if (const auto *InitStr =
                dyn_cast<ConstantDataArray>(Initializer->getOperand(0))) {
          if (InitStr->isString() &&
              // Drop last \00 symbol in the func name in selector
              F->getName().contains(InitStr->getAsString().drop_back(1))) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

void EscapeAnalysisGlobalInfo::traverseCGTopDown(
    const SmallVectorImpl<std::vector<CallGraphNode *>> &SCCList,
    const DenseMap<const Function *, SmallVector<const CallBase *>>
        &FuncCallSites,
    const SmallPtrSetImpl<const Function *> &RecursiveFuncs) {
  // Perform a top-down traversal of the call graph (from callers to callees)
  // and update argument escape status for each called function based on the
  // calling context of the caller functions.
  for (auto It = SCCList.rbegin(); It != SCCList.rend(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;

    bool Converged = false;
    while (!Converged) {
      Converged = true;
      for (const CallGraphNode *CGN : SCC) {
        Function *F = CGN->getFunction();

        // We consider only localy defined, static functions
        if (!isLocalAndExactFunc(F) ||
            (!TsanWholeProgram && !F->hasLocalLinkage()) ||
            // For now, conservatively skip all ObjC methods
            isFuncPassedToObjCSelector(F))
          continue;

        const auto &TLI = MAM.getResult<FunctionAnalysisManagerModuleProxy>(M)
                              .getManager()
                              .getResult<TargetLibraryAnalysis>(*F);

        LLVM_DEBUG(dbgs() << "\nIPATopDown Func: " << F->getName() << "\n");

        // For non-recursive functions, no need to iterate until convergence
        if ((SCC.size() == 1) && !RecursiveFuncs.contains(F)) {
          evalTopDownArgEscStatus(FuncCallSites, F);
          FuncEscapeInfo.erase(F);
          FuncEscapeInfo.try_emplace(
              F, EscapeAnalysisInfo(*F, TLI, NonEscapingFuncs, IPABottomTopEscInfo,
                                    IPATopDownArgEscInfo));
          break;
        }

        // Unless, check convergence
        const auto PrevIPAFuncInfoIt = IPATopDownArgEscInfo->find(F);
        SmallVector<bool> PrevIPAFuncInfo;
        if (PrevIPAFuncInfoIt != IPATopDownArgEscInfo->end())
          PrevIPAFuncInfo = PrevIPAFuncInfoIt->second;
        else
          Converged = false;

        // Build escape summary for a function
        evalTopDownArgEscStatus(FuncCallSites, F);
        FuncEscapeInfo.erase(F);
        FuncEscapeInfo.try_emplace(
            F, EscapeAnalysisInfo(*F, TLI, NonEscapingFuncs,
                                  IPABottomTopEscInfo, IPATopDownArgEscInfo));

        if (Converged && (PrevIPAFuncInfo != (*IPATopDownArgEscInfo)[F]))
          Converged = false;
      }
    }
  }
}

void EscapeAnalysisGlobalInfo::readNonEscapingFuncs() {
  // See TsanUseAnalysisSummaries: reuse across modules is opt-in, and the file
  // is read from the same directory it is written to. Previously the write
  // went to tsan-logs/ and the read looked in the working directory, so this
  // path only ever fired on a file someone had put there by hand.
  if (!TsanUseAnalysisSummaries)
    return;

  std::ifstream WhiteListFile;
  if (!openSummary(WhiteListFile, FuncWhiteListFileName)) {
    LLVM_DEBUG(dbgs() << "No usable non-escaping-argument summary\n");
    return;
  }
  SummaryLoaded = true;

  std::string FuncLine;
  while (std::getline(WhiteListFile, FuncLine)) {
    if (FuncLine.empty())
      continue;

    // Check if the function definition contains arguments that don't escape
    const auto ColonPos = FuncLine.find(':');
    if (ColonPos == std::string::npos) {
      // No argument list. All arguments are not escpaping
      if (NonEscapingFuncs->find(FuncLine) == NonEscapingFuncs->end())
        (*NonEscapingFuncs)[FuncLine] = SmallSet<unsigned, 4>();
      continue;
    }

    // Parse argument
    std::string FuncName = FuncLine.substr(0, ColonPos);
    // A summary speaks only for externally visible functions.
    if (const Function *F = M.getFunction(FuncName); F && F->hasLocalLinkage())
      continue;
    std::istringstream ArgStream(FuncLine.substr(ColonPos + 1));
    unsigned ArgInd;
    SmallSet<unsigned, 4> NonEscapingArgs;

    while (ArgStream >> ArgInd)
      NonEscapingArgs.insert(ArgInd);

    // Store the function name along with its non-escaping arguments
    (*NonEscapingFuncs)[FuncName] = std::move(NonEscapingArgs);

    // If the function already exists in NonEscapingFuncs, update its arguments
    auto ExistingArgsIt = NonEscapingFuncs->find(FuncName);
    ExistingArgsIt->second.insert(NonEscapingArgs.begin(),
                                  NonEscapingArgs.end());
  }
  WhiteListFile.close();
}

std::ofstream EscapeAnalysisGlobalInfo::EscFuncsFile;

static std::string getFileNameFromPath(std::string Path) {
  std::replace(Path.begin(), Path.end(), '/', '_');
  return Path;
}

// At call time, not static-initialisation time: the directory is a
// command-line option.
static void createLogDir() {
  const std::string LogDir = tsanSummaryDir();
  std::error_code EC;
  if (!std::filesystem::exists(LogDir))
    if (!std::filesystem::create_directories(LogDir, EC) && EC)
      errs() << "Error creating directory " << LogDir << ": " << EC.message()
             << "\n";
}

EscapeAnalysisGlobalInfo::EscapeAnalysisGlobalInfo(CallGraph &CG, Module &M_,
                                                   ModuleAnalysisManager &MAM_)
    : M(M_), MAM(MAM_) {
  DEBUG_WITH_TYPE(PRINT_ESCAPING_CALLEES,
    const auto FileName = tsanSummaryDir() + "/escaping_callees_" +
                          getFileNameFromPath(M.getName().str()) + ".txt";
    EscFuncsFile.open(FileName);
    if (!EscFuncsFile.is_open())
      dbgs() << "Warning: Could not open " << FileName << " for appending.\n"
  );

  readNonEscapingFuncs();

  // We do a bottom-up SCC traversal of the call graph.  In other words, we
  // visit all callees before callers (leaf-first).

  // This is needed to (conservatively) consider recursive calls and SCCs.
  // First, find all SCCs and set all pointer argument as escaped
  SmallPtrSet<const Function *, 8> RecursiveFuncs;
  traverseSCCsAndInitIPAEscInfo(CG, RecursiveFuncs);

  LLVM_DEBUG(printIPAFuncEscInfo(IPABottomTopEscInfo.get(), dbgs()));

  // We need it to traverse call graph in reverse (top-bottom) order later
  SmallVector<std::vector<CallGraphNode *>> SCCList;

  LLVM_DEBUG(dbgs() << "###################################################\n");
  LLVM_DEBUG(dbgs() << "####### IPA Bottom-Top Escape Analysis       ##### \n");
  LLVM_DEBUG(dbgs() << "###################################################\n");

  ////
  // First need to understand, how arguments can escape in calls. E.g.
  //   void foo(int *x) { bar(x); }
  //   void bar(int *y) { ... }
  // To understand, how x can escape in foo, we need to investigate
  // all calls with x as an argument. To this end, we traverse callgraph
  // from bar (callee) to foo (caller)
  traverseCGBottomTop(CG, RecursiveFuncs, SCCList);

  // For each function, get list of instruction calling it
  const auto FuncCallSites = getFuncToCallSitesMap();

  LLVM_DEBUG(dbgs() << "###################################################\n");
  LLVM_DEBUG(dbgs() << "######## IPA Top-Down Escape Analysis        ######\n");
  LLVM_DEBUG(dbgs() << "###################################################\n");

  ////
  // Then we try to understand how arguments can escape from functions
  // _being called_. E.g.
  // void foo(int *x) { ... }
  // void bar() { foo(y); }
  // To understand that, we traverse callgraph from top (callers, i.e. bar)
  // to bottom (calleee, i.e. foo)
  traverseCGTopDown(SCCList, FuncCallSites, RecursiveFuncs);

  LLVM_DEBUG(printArgEscStatus(););

  // Never over the summary this compile was seeded from.
  if (TsanUseAnalysisSummaries && !SummaryLoaded)
    writeIPASummary();

  DEBUG_WITH_TYPE(PRINT_ESCAPING_CALLEES,
      if (EscFuncsFile.is_open()) { EscFuncsFile.close(); });
}

/// For given pointer, get underlying objects, and get escape status for them
bool EscapeAnalysisGlobalInfo::isEscapedUndrlObjOrPointee(
    const Value *Addr, const TargetLibraryInfo &TLI, const BasicBlock *BB,
    EscapeAnalysisInfo::EscReasonTy &EscReason) {
  bool IsComplete = true;
  const SmallVector<UnderlObjTy> UnderlObjs =
      EscapeAnalysisInfo::getUnderlyingMayEscObjs(Addr, TLI,
                                                  EscapeAnalysisInfo::MaxUnderlObjLookup,
                                                  nullptr,
                                                  &IsComplete);
  // The object walk gave up: we do not know what this address refers to, so we
  // cannot claim it does not escape. Returning false here -- which an empty
  // list used to do silently -- dropped instrumentation for every access whose
  // base pointer the walk could not identify.
  if (!IsComplete) {
    LLVM_DEBUG(dbgs() << "isEscapedUndrlObjOrPointee: incomplete object list, "
                         "assuming escaped\n");
    EscReason = EscapeAnalysisInfo::OTHER;
    return true;
  }

  for (const UnderlObjTy &UnderlObj : UnderlObjs) {
    LLVM_DEBUG(dbgs() << "isEscapedUndrlObjOrPointee UnderlObj: "
                      << *UnderlObj.Obj << "\n");
    if (isEscapedForBBTSan(BB->getParent(), BB, UnderlObj, EscReason))
      return true;
  }
  return false;
}

bool EscapeAnalysisGlobalInfo::isEscapedForBBTSan(
    const Function *F, const BasicBlock *BB, const UnderlObjTy &UnderlObj,
    EscapeAnalysisInfo::EscReasonTy &EscReason) {
  const auto FEIIt = FuncEscapeInfo.find(F);
  // No summary for this function means no evidence that anything stays local,
  // which is the opposite of what returning false would say.
  if (FEIIt == FuncEscapeInfo.end())
    return true;
  if (EscapeAnalysisInfo::isNonConstGV(UnderlObj.Obj))
    return true;

  // Reasons accumulate over the object and each of its pointees. The
  // out-parameter used to be overwritten by every query, so an escaped slot
  // whose last visited pointee was local answered "not escaped".
  EscapeAnalysisInfo::EscReasonTy R;
  FEIIt->second.isEscapedForBBIPA(BB, UnderlObj, &R);
  EscReason |= R;

  // If object escapes by passing to a function, it doesn't matter whether
  // it's a pointer or not, because even pointer may escape through this
  // (but not only pointee object)
  if ((EscReason & EscapeAnalysisInfo::EscReasonTy(EscapeAnalysisInfo::PASSING_TO_CALL)).any())
    return true;

  if (UnderlObj.Loaded) {
    // A slot with no recorded pointee holds a pointer the analysis never saw
    // stored -- filled by memcpy, by a callee, by anything -- so what it
    // points to may be anywhere. Zero pointees is "escaped", not "local".
    unsigned NumPointees = 0;
    FEIIt->second.forEachPointeeDo(UnderlObj, BB, [&](const ObjAndPath &OAP) {
      ++NumPointees;
      EscapeAnalysisInfo::EscReasonTy RP;
      FEIIt->second.isEscapedForBBIPA(BB, OAP, &RP);
      EscReason |= RP;
    });
    if (NumPointees == 0 && ClEAUnseenPointeeEscapes) {
      ++NumEAUnseenPointeeLoads;
      EscReason |= EscapeAnalysisInfo::EscReasonTy(EscapeAnalysisInfo::OTHER);
    }
  }
  return EscReason.any();
}

bool EscapeAnalysisGlobalInfo::isEscapedUndrlObjOrPointeeAnywhere(
    const Value *Addr, const TargetLibraryInfo &TLI, const Function *F,
    EscapeAnalysisInfo::EscReasonTy &EscReason) {
  bool IsComplete = true;
  const SmallVector<UnderlObjTy> UnderlObjs =
      EscapeAnalysisInfo::getUnderlyingMayEscObjs(
          Addr, TLI, EscapeAnalysisInfo::MaxUnderlObjLookup, nullptr,
          &IsComplete);
  if (!IsComplete) {
    EscReason = EscapeAnalysisInfo::OTHER;
    return true;
  }
  const auto FEIIt = FuncEscapeInfo.find(F);
  if (FEIIt == FuncEscapeInfo.end())
    return true;
  for (const UnderlObjTy &UnderlObj : UnderlObjs) {
    if (EscapeAnalysisInfo::isNonConstGV(UnderlObj.Obj))
      return true;
    EscapeAnalysisInfo::EscReasonTy R;
    FEIIt->second.isEscapedInFuncIPA(F, UnderlObj, &R);
    EscReason |= R;
    if ((EscReason & EscapeAnalysisInfo::EscReasonTy(EscapeAnalysisInfo::PASSING_TO_CALL)).any())
      return true;
    if (UnderlObj.Loaded) {
      // Pointees are per block; take every block's. None anywhere: the
      // pointer came from somewhere the analysis never saw -- escaped.
      unsigned NumPointees = 0;
      for (const BasicBlock &BB : *F)
        FEIIt->second.forEachPointeeDo(
            UnderlObj, &BB, [&](const ObjAndPath &OAP) {
              ++NumPointees;
              EscapeAnalysisInfo::EscReasonTy RP;
              FEIIt->second.isEscapedInFuncIPA(F, OAP, &RP);
              EscReason |= RP;
            });
      if (NumPointees == 0 && ClEAUnseenPointeeEscapes)
        EscReason |=
            EscapeAnalysisInfo::EscReasonTy(EscapeAnalysisInfo::OTHER);
    }
    if (EscReason.any())
      return true;
  }
  return false;
}

void EscapeAnalysisGlobalInfo::print(Module &M, raw_ostream &O) const {
  for (const Function &F : M) {
    if (!isLocalAndExactFunc(&F))
      continue;

    const auto It = FuncEscapeInfo.find(&F);
    assert(It != FuncEscapeInfo.end() && "Function EA results must exist\n");
    O << "Printing analysis 'Escape Analysis' for function '" << F.getName()
      << "':\n";
    It->second.print(O);
  }
}

void EscapeAnalysisGlobalInfo::writeIPASummary() {
  createLogDir();
  const auto SummaryFileName = tsanSummaryDir() + "/" + FuncWhiteListFileName;
  std::ofstream SummaryFile(SummaryFileName, std::ios::out);
  if (!SummaryFile.is_open()) {
    errs() << "Error opening summary file: " << SummaryFileName << "\n";
    return;
  }
  // Only externally visible functions, sorted: a local one is private to its
  // unit and llvm-link renames the clashing ones.
  SummaryFile << summaryHeader();
  std::vector<std::string> Lines;
  for (const auto &Entry : *IPABottomTopEscInfo) {
    if (!Entry.first->hasName() || Entry.first->hasLocalLinkage())
      continue;
    std::string Line;
    for (const auto &ArgEscapeEntry : Entry.second.ArgEscapes) {
      // The question is whether the callee lets the pointer escape; that an
      // argument aliases its caller's memory is what an argument is, not an
      // escape, and every pointer argument carries that bit.
      const auto Reason =
          ArgEscapeEntry.second &
          ~EscapeAnalysisInfo::EscReasonTy(
              EscapeAnalysisInfo::EscReasonBits::PTR_ARG_ALIASING);
      if (!Reason.none())
        continue;
      if (Line.empty())
        Line = Entry.first->getName().str() + ":";
      Line += " " + std::to_string(ArgEscapeEntry.first);
    }
    if (!Line.empty())
      Lines.push_back(Line);
  }
  llvm::sort(Lines);
  for (const auto &L : Lines)
    SummaryFile << L << "\n";
  SummaryFile.close();
}

AnalysisKey EscapeAnalysisGlobal::Key;

EscapeAnalysisGlobal::Result
EscapeAnalysisGlobal::run(Module &M, ModuleAnalysisManager &AM) {
  return EscapeAnalysisGlobalInfo(AM.getResult<CallGraphAnalysis>(M), M, AM);
}

PreservedAnalyses
EscapeAnalysisGlobalPrinterPass::run(Module &M,
                                     ModuleAnalysisManager &AM) const {
  OS << "Printing analysis 'Escape Analysis' for module '" << M.getName()
     << "':\n";
  AM.getResult<EscapeAnalysisGlobal>(M).print(M, OS);
  return PreservedAnalyses::all();
}

//===----------------------------------------------------------------------===//
// getUnderlyingObject infrastructure (taken and modified from ValueTracker.cpp)
//===----------------------------------------------------------------------===//

/// Check if it's GEP accessing the structure field
static bool isStructFieldGEP(const GEPOperator *GEP) {
  // First index is 0, the next ones corresponds the fields
  if ((GEP->getNumIndices() < 2) || !GEP->hasAllConstantIndices())
    return false;

  // Get the first index (after the pointer).
  // In the case of structure field access, the first index should usually be 0.
  Value *FirstIndex = GEP->getOperand(1);

  // Strip away potential type casts to get to the base value.
  FirstIndex = FirstIndex->stripPointerCasts();

  // Check if the first index is a constant 0.
  if (const auto *ConstIntFirstIndex = dyn_cast<ConstantInt>(FirstIndex)) {
    if (!ConstIntFirstIndex->isZero()) {
      return false; // First index is not 0, probably not a structure field
                    // access.
    }
  } else {
    return false; // First index is not a constant, not suitable for direct
                  // structure field access.
  }

  // Check the type of the pointer to which the GEP is applied.
  // It should be a pointer to a structure or a pointer to a pointer to a
  // structure, etc.
  Type *PtrType = GEP->getSourceElementType();

  // Check if the base type is a structure.
  if (isa<StructType>(PtrType))
    return true; // Looks like a structure field access.

  return false;
}

/// Get underlying object and record the path to the accessed field (if exists)
static const Value *getUnderlyingObjectWithPath(const Value *V,
                                                unsigned MaxLookup,
                                                SmallVector<unsigned> &Path) {
  if (!V->getType()->isPointerTy())
    return V;

  // Is all the indices are structure field accesses
  bool IsAllStructFieldIndices = true;
  for (unsigned Count = 0; MaxLookup == 0 || Count < MaxLookup; ++Count) {
    if (auto *GEP = dyn_cast<GEPOperator>(V)) {
      V = GEP->getPointerOperand();
      if (IsAllStructFieldIndices) {
        if (isStructFieldGEP(GEP)) {
          // if (Path == EmptyFieldPath)
          // Path = SmallVector<unsigned>();

          // Add indices from GEP to Path (except the very first index)
          for (unsigned i = 2; i < GEP->getNumOperands(); ++i) {
            const auto *ConstIntIndex = cast<ConstantInt>(GEP->getOperand(i));
            Path.push_back(ConstIntIndex->getZExtValue());
          }
        } else {
          if (Path != EmptyFieldPath)
            Path = EmptyFieldPath;
          IsAllStructFieldIndices = false;
        }
      }
    } else if (Operator::getOpcode(V) == Instruction::BitCast ||
               Operator::getOpcode(V) == Instruction::AddrSpaceCast ||
               Operator::getOpcode(V) == Instruction::Freeze) {
      V = cast<Operator>(V)->getOperand(0);
      if (!V->getType()->isPointerTy())
        return V;
    } else if (auto *GA = dyn_cast<GlobalAlias>(V)) {
      if (GA->isInterposable())
        return V;
      V = GA->getAliasee();
    } else {
      if (auto *PHI = dyn_cast<PHINode>(V)) {
        // Look through single-arg phi nodes created by LCSSA.
        if (PHI->getNumIncomingValues() == 1) {
          V = PHI->getIncomingValue(0);
          continue;
        }
      } else if (auto *Call = dyn_cast<CallBase>(V)) {
        // CaptureTracking can know about special capturing properties of some
        // intrinsics like launder.invariant.group, that can't be expressed with
        // the attributes, but have properties like returning aliasing pointer.
        // Because some analysis may assume that nocaptured pointer is not
        // returned from some special intrinsic (because function would have to
        // be marked with returns attribute), it is crucial to use this function
        // because it should be in sync with CaptureTracking. Not using it may
        // cause weird miscompilations where 2 aliasing pointers are assumed to
        // noalias.
        if (auto *RP = getArgumentAliasingToReturnedPointer(Call, false)) {
          V = RP;
          continue;
        }
      }

      return V;
    }
    assert(V->getType()->isPointerTy() && "Unexpected operand type!");
  }
  return V;
}

/// Wrapper around getUnderlyingObject to look through loads
static UnderlObjTy getUnderlObjThroughLoads(const Value *&P,
                                            const unsigned MaxLookup) {
  SmallVector<unsigned> Path;
  bool LoadInstFlag = false;
  while (true) {
    P = getUnderlyingObjectWithPath(P, MaxLookup, Path);
    if (const auto *Load = dyn_cast<LoadInst>(P)) {
      P = Load->getPointerOperand();
      LoadInstFlag = true;
    } else {
      return {{P, Path}, LoadInstFlag}; // INFO: Path
    }
  }
}

/// This method is similar to getUnderlyingObject except that it can
/// look through phi and select instructions and return multiple objects.
///
/// This is slightly modified version from ValueTracking.cpp. The differences:
/// 1. Pass through LoadInst to get the original loaded object.
/// 2. Ignore phi invariant check.
static void
getUnderlObjsWithoutPHIInvCheck(const Value *V,
                                SmallVectorImpl<UnderlObjTy> &Objects,
                                const unsigned MaxLookup) {
  SmallPtrSet<const Value *, 4> Visited;
  SmallVector<UnderlObjTy> Worklist;
  Worklist.push_back({{V, EmptyFieldPath}, false}); // INFO: Path
  do {
    const Value *Obj = Worklist.pop_back_val().Obj;
    UnderlObjTy UO = getUnderlObjThroughLoads(Obj, MaxLookup);

    if (!Visited.insert(UO.Obj).second)
      continue;

    // INFO use "&" operator for LoadFlag to combine loads
    if (auto *SI = dyn_cast<SelectInst>(UO.Obj)) {
      Worklist.push_back({{SI->getTrueValue(), EmptyFieldPath}, UO.Loaded}); // INFO: Path
      Worklist.push_back({{SI->getFalseValue(), EmptyFieldPath}, UO.Loaded}); // INFO: Path
      continue;
    }

    if (auto *PN = dyn_cast<PHINode>(UO.Obj)) {
      // In original function, we check here whether PHI is invariant during
      // the loop. In this version, we are conservative and ignore it.
      // append_range(Worklist, PN->incoming_values());
      for (const Value *Incoming : PN->incoming_values())
        Worklist.push_back({{Incoming, EmptyFieldPath}, UO.Loaded}); // INFO: Path;
      continue;
    }

    Objects.push_back(UO);
  } while (!Worklist.empty());
}

/// This is the function that does the work of looking through basic
/// ptrtoint+arithmetic+inttoptr sequences.
static const Value *getUnderlyingObjectFromInt(const Value *V) {
  do {
    if (const Operator *U = dyn_cast<Operator>(V)) {
      // If we find a ptrtoint, we can transfer control back to the
      // regular getUnderlyingObjectFromInt.
      if (U->getOpcode() == Instruction::PtrToInt)
        return U->getOperand(0);
      // If we find an add of a constant, a multiplied value, or a phi, it's
      // likely that the other operand will lead us to the base
      // object. We don't have to worry about the case where the
      // object address is somehow being computed by the multiply,
      // because our callers only care when the result is an
      // identifiable object.
      if (U->getOpcode() != Instruction::Add ||
          (!isa<ConstantInt>(U->getOperand(1)) &&
           Operator::getOpcode(U->getOperand(1)) != Instruction::Mul &&
           !isa<PHINode>(U->getOperand(1))))
        return V;
      V = U->getOperand(0);
    } else {
      return V;
    }
    assert(V->getType()->isIntegerTy() && "Unexpected operand type!");
  } while (true);
}

/// This is a wrapper around getUnderlyingObjects and adds support for basic
/// ptrtoint+arithmetic+inttoptr sequences.
/// It returns false if unidentified object is found in getUnderlyingObjects.
static bool getUnderlObjsForCodeGenWithoutPHIInvCheck(
    const Value *V, const TargetLibraryInfo &TLI,
    SmallVectorImpl<UnderlObjTy> &Objects, const unsigned MaxLookup,
    std::shared_ptr<EscapeAnalysisInfo::IPABottomTopMap> IPAFuncEscInfo) {
  SmallPtrSet<const Value *, 16> Visited;
  SmallVector<const Value *, 4> Working(1, V);
  do {
    V = Working.pop_back_val();

    SmallVector<UnderlObjTy> Objs;
    getUnderlObjsWithoutPHIInvCheck(V, Objs, MaxLookup);

    for (const UnderlObjTy &UO : Objs) {
      if (!Visited.insert(UO.Obj).second)
        continue;
      if (Operator::getOpcode(UO.Obj) == Instruction::IntToPtr) {
        const Value *OWithoutCast =
            getUnderlyingObjectFromInt(cast<User>(UO.Obj)->getOperand(0));

        // Pass through loads
        const auto UnderlObj =
            getUnderlObjThroughLoads(OWithoutCast, MaxLookup);

        if (UnderlObj.Obj->getType()->isPointerTy() ||
            isa<PHINode>(UnderlObj.Obj)) {
          Working.push_back(UnderlObj.Obj);
          continue;
        }
      }

      // If getUnderlyingObjects fails to find an identifiable object,
      // getUnderlyingObjectsForCodeGen also fails for safety.
      // An object the walk cannot vouch for: not an identified object, not an
      // argument, not a call result. A pointer-returning call is always an
      // object -- fresh memory if its callee returns nothing escaped (then it
      // is tracked like an alloca), an escaped call otherwise (getExtObjStatus
      // says so). It used to count as unresolvable whenever the callee
      // returned fresh memory, which dropped the publication of such memory
      // in the transfer function and, once an unresolvable operand meant
      // "anything", poisoned whole functions.
      // Null and undef are not objects: nothing is reached through them, so
      // a store, call or return of one publishes nothing. They used to count
      // as unresolvable (34,030 of the 59,028 unresolved operands on
      // sqlite3.c were null), and an unresolvable operand means "anything".
      if (isa<ConstantPointerNull>(UO.Obj) || isa<UndefValue>(UO.Obj))
        continue;
      if (!isIdentifiedObject(UO.Obj) && !isa<Argument>(UO.Obj) &&
          !isa<CallBase>(UO.Obj)) {
        LLVM_DEBUG(dbgs() << "UNRESOLVED-OBJ " << *UO.Obj << "\n");
        if (isa<IntToPtrInst>(UO.Obj)) ++NumEAUnresolvedIntToPtr;
        else if (isa<ConstantPointerNull>(UO.Obj)) ++NumEAUnresolvedNull;
        else if (isa<UndefValue>(UO.Obj)) ++NumEAUnresolvedUndef;
        else if (isa<ConstantExpr>(UO.Obj)) ++NumEAUnresolvedConstExpr;
        else if (isa<PHINode>(UO.Obj)) ++NumEAUnresolvedPHI;
        else if (isa<SelectInst>(UO.Obj)) ++NumEAUnresolvedSelect;
        else if (isa<LoadInst>(UO.Obj)) ++NumEAUnresolvedLoad;
        else if (isa<ExtractValueInst>(UO.Obj)) ++NumEAUnresolvedExtractValue;
        else if (isa<GetElementPtrInst>(UO.Obj)) ++NumEAUnresolvedGEP;
        else ++NumEAUnresolvedOther;
        Objects.clear();
        return false;
      }
      Objects.push_back(UO);
    }
  } while (!Working.empty());
  return true;
}

/// Recuresively search in the instruction for the underlying objects which
/// may escape
SmallVector<UnderlObjTy> EscapeAnalysisInfo::getUnderlyingMayEscObjs(
    const Value *V, const TargetLibraryInfo &TLI, const unsigned MaxLookup,
    std::shared_ptr<IPABottomTopMap> IPAFuncEscInfo, bool *IsComplete) {
  SmallVector<UnderlObjTy> UnderlObjs;
  const bool Complete = getUnderlObjsForCodeGenWithoutPHIInvCheck(
      V, TLI, UnderlObjs, MaxLookup, IPAFuncEscInfo);
  if (IsComplete)
    *IsComplete = Complete;
  LLVM_DEBUG(dbgs() << "\tgetUnderlyingMayEscObjects for " << *V << "\n");
  LLVM_DEBUG(if (!UnderlObjs.empty()) {
    dbgs() << "\tgetUnderlyingMayEscObjects:";
    for (const auto &Obj : UnderlObjs) {
      dbgs() << "\t\t" << *Obj.Obj << "\n\t\t\tPath: ";
      if (Obj.Path != EmptyFieldPath)
        for (unsigned Index : Obj.Path)
          dbgs() << Index << " ";
      else
        dbgs() << "None";
      dbgs() << "\n";
    }
  } else dbgs() << "\tgetUnderlyingMayEscObjects -- empty\n";);
  return UnderlObjs;
}
