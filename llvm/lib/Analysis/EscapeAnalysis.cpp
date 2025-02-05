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

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Demangle/Demangle.h"

#include <deque>

using namespace llvm;

#define DEBUG_TYPE "ea"
#define DEEP_DEBUG_TYPE "ea-deep-debug"

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

[[maybe_unused]] static void dbgPrintOpnd(const Use &Opnd) {
  dbgs() << "\n\tOPND \t";
  if (const auto F = dyn_cast<Function>(Opnd.get()))
    dbgs() << F->getName() << "\t";
  else if (const auto *BB = dyn_cast<BasicBlock>(Opnd.get()))
    dbgs() << BB->getName() << "\t";
  else
    dbgs() << *Opnd.get() << "\t";
}

static void dbgPrintAliasCand(const SmallVectorImpl<UnderlObjInfo> &UnderlObjs) {
  for (const auto &A : UnderlObjs)
    dbgs() << "\tAlias candidate: " << *A.Obj << "\n";
}

void EscapeAnalysisGlobalInfo::printArgEscStatus() {
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

void EscapeAnalysisInfo::EscapeState::checkAndUpdEscStatus(
    const Value *CheckedObj, const Value *AffectedObj,
    const EscapeAnalysisInfo *EAI) {
  // If instruction creates an alias to the object which has escaped before
  // or escapes "by definition" (e.g. pointer function argument,
  // global pointer), then that's not just aliasing, but escaping as well

  LLVM_DEBUG(dbgs() << "\tcheckAndUpdEscStatus: " << *CheckedObj << " --> "
                    << *AffectedObj << "\n");
  if (const auto EscReason = EAI->getExtObjStatusWithIPA(CheckedObj);
      EscReason.any())
    addEscapeObjOrReason(AffectedObj, EscReason);

  // Assigning to structures
  if (const auto *Alloca = dyn_cast<AllocaInst>(CheckedObj);
      Alloca && Alloca->getAllocatedType()->isStructTy()) {
    if (const auto It = EscapedObjs.find(AffectedObj); It != EscapedObjs.end())
      addEscapeObjOrReason(CheckedObj, It->second);

    if (const auto EscReason = EAI->getExtObjStatusWithIPA(AffectedObj);
        EscReason.any())
      addEscapeObjOrReason(CheckedObj, EscReason);
  }
}

void EscapeAnalysisInfo::EscapeState::forEachPointeeDo(
    const Value *Obj, std::function<void(const Value *)> Action) const {
  LLVM_DEBUG(dbgs() << "\t\t\t\tforEachPointeeDo: " << *Obj << "\n";);
  const auto Pointees = PointsTo.getPointees(Obj);

  if (const auto Pointees = PointsTo.getPointees(Obj); Pointees)
    for (const Value *Pointee : Pointees.value())
      Action(Pointee);
}

/// Add the alias: Alias --> PointeeValue
void EscapeAnalysisInfo::EscapeState::addPointsTo(
    const UnderlObjInfo &Pointer, const UnderlObjInfo &Pointee,
    const EscapeAnalysisInfo *EAI) {
  assert(Pointer.Obj->getType()->isPointerTy() && "Alias must be a pointer\n");

  if ((Pointer.Obj == Pointee.Obj) || (Pointee.Obj == nullptr) ||
      PointsTo.PointsToMap[Pointer.Obj].contains(Pointee.Obj))
    return;

  LLVM_DEBUG(dbgs() << "\taddPointsTo: " << *Pointer.Obj << " --> "
                    << *Pointee.Obj << "\n");
  // LLVM_DEBUG(dbgs() << "\t\tPointer.Loaded: " << Pointer.Loaded
                    // << "\n\t\tPointee.Loaded: " << Pointee.Loaded << "\n");

  PointsTo.PointsToMap[Pointer.Obj].insert(Pointee.Obj);

  if (Pointee.Loaded) {
    LLVM_DEBUG(dbgs() << "\t\t\tPointee loaded\n";);
    forEachPointeeDo(Pointee.Obj, [&](const Value *Ptee) {
      LLVM_DEBUG(dbgs() << "\t\t\t\tforEachPointeeDo: " << *Ptee << "\n");
      addPointsTo(Pointer, {Ptee, false}, EAI);
    });
    return;
  }

  checkAndUpdEscStatus(Pointer.Obj, Pointee.Obj, EAI);

  // If pointer value was loaded, we should check whether pointee object is
  // escaped or not
  if (Pointer.Loaded) {
    LLVM_DEBUG(dbgs() << "\t\t\tPointer loaded\n";);
    forEachPointeeDo(Pointer.Obj, [&](const Value *Ptee) {
      checkAndUpdEscStatus(Ptee, Pointee.Obj, EAI);
    });
  }

  // If pointee object is escaped (as observed from previous analysis)
  // if (const auto It = EscapedObjs.find(Pointer.Obj); It != EscapedObjs.end())
    // addEscapeObjOrReason(Pointee.Obj, It->second);

  // Considering transitivity: recursively add new alias to all existing aliases
  // of PointeeValue
  forEachPointeeDo(Pointee.Obj, [&](const Value *Ptee) {
    addPointsTo(Pointer, {Ptee, false}, EAI);
  });
}

void EscapeAnalysisInfo::EscapeState::print(raw_ostream &OS) const {
  OS << "Escaped objects:\n";
  for (auto &Obj : EscapedObjs) {
    OS << "  " << *Obj.first << " : ";
    printEscReason(Obj.second);
    OS << "\n";
  }
}

EscapeAnalysisInfo::EscReasonTy EscapeAnalysisInfo::EscapeState::getEscReason(
    const Value *V) const {
  const auto EscObjsIt = EscapedObjs.find(V);
  if (EscObjsIt != EscapedObjs.end())
    return EscObjsIt->second;
  return 0;
}

/// Get list of aliases for the object a
std::optional<EscapeAnalysisInfo::PointsToRelTy::PointeeListTy>
EscapeAnalysisInfo::PointsToRelTy::getPointees(const Value *V) const {
  const auto It = PointsToMap.find(V);
  if (It == PointsToMap.end())
    return std::nullopt;
  return It->second;
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
  for (const auto &[Key, ValueSet] : PointsToMap) {
    OS << "\tAlias: " << *Key << "\n";
    for (const Value *Alias : ValueSet)
      OS << "\t\t --> " << *Alias << "\n";
  }
  OS << "\n";
}

//===----------------------------------------------------------------------===//
// EscapeState
//===----------------------------------------------------------------------===//

bool EscapeAnalysisInfo::EscapeState::operator==(const EscapeState &ES) const {
  if (this == &ES)
    return true;
  return ((EscapedObjs == ES.EscapedObjs) && (PointsTo == ES.PointsTo));
}

void EscapeAnalysisInfo::EscapeState::addEscapeObjOrReason(
    const Value *EscObj, const EscReasonTy EscReason) {
  LLVM_DEBUG(dbgs() << "\t\t\t\taddEscapeObjOrReason: " << *EscObj << "\n"
                    << "\t\t\t\tNew EscReason: "; printEscReason(EscReason););
  if (const auto EscObjIt = EscapedObjs.find(EscObj);
      EscObjIt != EscapedObjs.end()) {
    // Object is already escaped - add the escape reason
    EscObjIt->second |= EscReason;
  } else {
    // Object has not escaped before - add it
    EscapedObjs.insert({EscObj, EscReason});
  }
}

void EscapeAnalysisInfo::EscapeState::addEscapingObject(
    const Value *EscObj, const EscReasonTy EscReason) {
  LLVM_DEBUG(dbgs() << "\t\taddEscapingObject: " << *EscObj << "\n");
  SmallVector<const Value *, 8> WorkList;
  SmallPtrSet<const Value *, 8> Visited;

  WorkList.push_back(EscObj);

  while (!WorkList.empty()) {
    const Value *Current = WorkList.pop_back_val();

    if (!Visited.insert(Current).second)
      continue;

    addEscapeObjOrReason(Current, EscReason);

    if (const auto Aliases = PointsTo.getPointees(Current);
        Aliases.has_value())
      for (const Value *Alias : Aliases.value())
        if (!Visited.contains(Alias))
          WorkList.push_back(Alias);
  }
}

void EscapeAnalysisInfo::EscapeState::mergeAliases(
    const EscapeState &OtherES, const EscapeAnalysisInfo *EAI) {
  for (const auto &[OtherKey, OtherValueSet] : OtherES.PointsTo.PointsToMap)
    for (const auto *OtherPointeeValue : OtherValueSet)
      // addAlias(OtherKey, OtherPointeeValue, EAI);
      PointsTo.PointsToMap[OtherKey].insert(OtherPointeeValue);
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
EscapeAnalysisInfo::getArgEscStatus(const unsigned ArgNo,
                                    const Function *Func) const {
  if (const auto FuncIt = IPABottomTopInfo->find(Func);
      FuncIt != IPABottomTopInfo->end()) {
    if (const auto ArgEscIt = FuncIt->second.ArgEscapes.find(ArgNo);
        ArgEscIt != FuncIt->second.ArgEscapes.end())
      return ArgEscIt->second & EscReasonTy(~EscReasonBits::PTR_ARG_ALIASING);
  }
  return EscReasonBits::PTR_ARG_ALIASING;
}

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::getArgEscTopDownIPA(
  const unsigned ArgNo, const Function *Func) const {
  if (const auto It = IPATopDownArgEsc->find(Func);
      It != IPATopDownArgEsc->end()) {
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
  if (const auto *CI = dyn_cast<CallInst>(V);
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

static bool isSafeExternalCall(StringRef FuncName) {
  return (FuncName == "malloc" || FuncName == "calloc" ||
          FuncName == "realloc" || FuncName == "strlen" ||
          FuncName == "strcmp" || FuncName == "memchr");
}

/// Check if it's a function call which can escape
static bool isCallMayEscape(const Value *V,
                            std::shared_ptr<EscapeAnalysisInfo::IPABottomTopMap>
                                IPABottomTopEscInfo = nullptr) {
  if (const auto *CB = dyn_cast<CallBase>(V); CB) {
    LLVM_DEBUG(dbgs() << "\t\tisCallMayEscape: " << *V << "\n";);
    if (isa<MemIntrinsic>(V))
      return false; // Intrinsics do not escape.

    // Check if the call is to a known memory allocation function.
    if (const Function *F = CB->getCalledFunction()) {
      if (F->isDeclaration()) {
        if (isSafeExternalCall(F->getName()))
          return false; // Memory allocation functions do not escape.

        // TODO Demangle the function name before comparison
        // const auto DemangledName = demangle(F->getName().str());
        // dbgs() << "\t\t\t\tDemangledName: " << DemangledName << "\n";
        // if (DemangledName == "operator new" ||
            // DemangledName == "operator new[]")
          // return false; // Memory allocation functions do not escape.
        // return true; // Unknown external function.
      }

      // Check if this function returns escaped value
      if (IPABottomTopEscInfo)
        return getIPAFuncRetEscStatus(IPABottomTopEscInfo, F);
    }
    return true;
  }
  return false;
}

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::getExtObjStatusWithIPA(const Value *V) const {
  const auto EscReason = getExtObjStatus(V);

  if (IPATopDownArgEsc && (EscReason == EscReasonBits::PTR_ARG_ALIASING)) {
    const auto *Arg = cast<Argument>(V);
    const auto EscReason =
        getArgEscTopDownIPA(Arg->getArgNo(), Arg->getParent());
    // LLVM_DEBUG(dbgs() << "\t\t\t\tgetArgEscTopDownIPA: ";
               // printEscReason(EscReason););
    return EscReason;
  }

  if (IPABottomTopInfo && (EscReason == EscReasonBits::ESCAPED_CALL)) {
    if (isCallMayEscape(V, IPABottomTopInfo))
      return EscReasonBits::ESCAPED_CALL;
    return EscReasonBits::NO_ESCAPE;
  }

  return EscReason;
}


//===----------------------------------------------------------------------===//
// Main analysis
//===----------------------------------------------------------------------===//

EscapeAnalysisInfo::EscapeAnalysisInfo(
    const Function &Fn, std::shared_ptr<IPABottomTopMap> IPABottomTopInfo_,
    std::shared_ptr<IPAArgEscFromCallsMap> IPAArgEscFromCallers_)
  : AnalyzedFunc(Fn), IPABottomTopInfo(IPABottomTopInfo_),
    IPATopDownArgEsc(IPAArgEscFromCallers_) {
  LLVM_DEBUG(dbgs() << "\n|||||||||||||||||||||||||||||||||||||||||||||||||||||"
                       "|||||||||||||||||\n"
                       "|||||||||||||||||||| Func "
                    << Fn.getName()
                    << "\t||||||||||||||||||||||\n"
                       "|||||||||||||||||||||||||||||||||||||||||||||||||||||||"
                       "|||||||||||||||\n");
  std::deque<const BasicBlock *> WorkList;

  // Traverse CFG in reverse post-order
  ReversePostOrderTraversal<const Function *> RPOT(&AnalyzedFunc);
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
    EscapeState &ES, const SmallVectorImpl<UnderlObjInfo> &UnderlObjs) {
  for (const auto EO : UnderlObjs) {
    LLVM_DEBUG(dbgs() << "\t\treturn: check: " << *EO.Obj << "\n";
               print(dbgs()));
    if (isEscapedForFunc(EO.Obj) || ES.getEscReason(EO.Obj).any()) {
      LLVM_DEBUG(dbgs() << "\t\t\t\treturn: " << *EO.Obj << " is escaped\n");
      IsRetEscape = true;
    } else
      LLVM_DEBUG(dbgs() << "\t\t\t\treturn: " << *EO.Obj
                        << " is not escaped\n");
  }
}

void EscapeAnalysisInfo::addEscapedPtrArgs(EscapeState &ES) {
  // Iterate over the arguments of the function
  for (const Argument &Arg : AnalyzedFunc.args()) {
    if (!Arg.getType()->isPointerTy())
      continue;

    // Check if the argument is escaped using getExtObjStatusWithIPA
    const auto ArgEscReason = getExtObjStatusWithIPA(&Arg);
    if (ArgEscReason.any()) {
      LLVM_DEBUG(dbgs() << "Argument " << Arg.getName() << " is escaped: ";
                 printEscReason(ArgEscReason););
      ES.addEscapingObject(&Arg, ArgEscReason);
    }
  }
}

/// Compute the resulting escape state for BB
void EscapeAnalysisInfo::compBBEscapeState(const BasicBlock *BB,
                                           EscapeState &ES) {
  if (BB->isEntryBlock()) {
    // if ((IPABottomTopInfo && (*IPABottomTopInfo)[&AnalyzedFunc].IsRecursive))
      // addEscapedPtrArgs(ES);
    if (IPABottomTopInfo)
      addEscapedPtrArgs(ES);
  }

  for (const Instruction &I : *BB) {
    LLVM_DEBUG(dbgs() << "\nI " << I << "\n");
    for (const Use &Opnd : I.operands()) {
      LLVM_DEBUG(dbgPrintOpnd(Opnd););

      const auto [EscKind, EscDetails] = getEscInfoForOpnd(Opnd);

      if (EscKind == EscKindTy::NO_ESCAPE)
        continue;

      assert(EscDetails.has_value() && "EscDetails must be set");

      //////////////////////////////////////////
      // SmallVector<Value *, 8> UObjs;
      // dbgs() << "\t\tgetUnderlyingObjectsForCodeGen for Opnd: " << *Opnd
      //        << "\n";
      // getUnderlyingObjectsForCodeGen(Opnd.get(), UObjs);
      // if (!UObjs.empty())
      //   for (const Value *UObj : UObjs)
      //     dbgs() << "\t\tUObj: " << *UObj << "\n";
      //////////////////////////////////////////

      auto UnderlObjs = getUnderlyingMayEscObjs(
          Opnd.get(), MaxUnderlObjLookup, IPABottomTopInfo);

      if (UnderlObjs.empty())
        continue;

      if (EscKind == EscKindTy::MAY_ESCAPE) {
        LLVM_DEBUG(dbgs() << "\t-- MAY_ESCAPE --\n");
        assert(std::holds_alternative<EscReasonTy>(EscDetails.value()) &&
          "getEscapeKindForOpnd must return escape reason");
        const auto EscReason = std::get<EscReasonTy>(EscDetails.value());

        // Yes, looks ugly, but calls returning pointer is the only case when
        // register escapes, not a memory
        if (EscReason == EscReasonBits::ESCAPED_CALL) {
          UnderlObjs.clear();
          UnderlObjs.push_back({&I, false});
        }

        // If that's return instruction, we should check if it can return
        // a pointer to some external object
        if ((I.getOpcode() == Instruction::Ret) && (!IsRetEscape))
          updRetEscStatus(ES, UnderlObjs);

        for (const auto &[Obj, Loaded] : UnderlObjs) {
          if (Loaded) {
            LLVM_DEBUG(dbgs() << "\t\tLoaded\n");
            ES.forEachPointeeDo(Obj, [&](const Value *Pointee) {
              ES.addEscapeObjOrReason(Pointee, EscReason);
            });
            continue;
          }

          const auto ExtEscReason = getExtObjStatus(Obj);
          LLVM_DEBUG(dbgs() << "\t\tEscObj: " << *Obj << "\n"
                     << "\t\tExtEscReason: "; printEscReason(ExtEscReason););
          if (ExtEscReason != EscReasonBits::GPTR_ALIASING)
            ES.addEscapingObject(Obj, EscReason);
        }
      } else { assert(EscKind == EscKindTy::MAY_ALIASING);
        assert((std::holds_alternative<SmallVector<UnderlObjInfo>>(
          EscDetails.value()) &&
          "getEscapeKindForOpnd must return alias list"));
        const auto AliasList =
            std::get<SmallVector<UnderlObjInfo>>(EscDetails.value());

        LLVM_DEBUG(dbgs() << "\t-- ALIASING --\n";
                   dbgPrintAliasCand(AliasList); );

        for (const auto &Alias : AliasList)
          for (const auto &Pointee : UnderlObjs)
            ES.addPointsTo(Alias, Pointee, this);
      }
    }
  }
}

EscapeAnalysisInfo::EscapeState EscapeAnalysisInfo::mergePredEscapeStates(
    const BasicBlock *BB) {
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
bool EscapeAnalysisInfo::structContainsPointerType(const Type *Ty) {
  if (Ty->isPointerTy()) return true;
  if (!Ty->isStructTy()) return false;

  for (const Type *EltTy : Ty->subtypes())
    if (structContainsPointerType(EltTy))
      return true;
  return false;
}

/// Escaping state for the function is the escape state for Exit BB
const EscapeAnalysisInfo::EscapedObjectsTy &EscapeAnalysisInfo::getFuncEscState() const {
  const auto It = BBEscapeStates.find(&AnalyzedFunc.back());
  assert(It != BBEscapeStates.end() &&
         "Escape state for exit  block  not  found");
  return It->second.getEscapedObjs();
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

  if (isCallMayEscape(Call, IPABottomTopInfo) && Call->getType()->isPointerTy())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::ESCAPED_CALL};

  // The pointer is not captured if returned pointer is not captured.
  // NOTE: CaptureTracking users should not assume that only functions
  // marked with nocapture do not capture. This means that places like
  // getUnderlyingObject in ValueTracking or DecomposeGEPExpression
  // in BasicAA also need to know about this property.
  if (isIntrinsicReturningPointerAliasingArgumentWithoutCapturing(Call, true))
    return {EscKindTy::MAY_ALIASING, getUnderlyingMayEscObjs(I)};

  // Volatile operations effectively capture the memory location that they
  // load and store to.
  if (const auto *MI = dyn_cast<MemIntrinsic>(Call)) {
    if (MI->isVolatile())
      return {EscKindTy::MAY_ESCAPE, EscReasonTy(EscReasonBits::VOLATILE)};

    const auto *Src = MI->getArgOperand(1);
    const auto *Dst = MI->getArgOperand(0);

    // Considering llvm.memcpy intrinsic
    if ((MI->getIntrinsicID() == Intrinsic::memcpy) && (Src == U.get()))
      // Check whether the source argument is a struct containing pointers
      if (const auto *Alloca = dyn_cast<AllocaInst>(U.get()))
        if (const Type *StructTy = Alloca->getAllocatedType();
            StructTy && structContainsPointerType(StructTy))
          return {EscKindTy::MAY_ALIASING, getUnderlyingMayEscObjs(Dst)};
  }

  if (isSafeExternalCall(Call->getCalledFunction()->getName()))
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  // Calling a function pointer does not in itself cause the pointer to
  // be captured.  This is a subtle point considering that (for example)
  // the callee might return its own address.  It is analogous to saying
  // that loading a value from a pointer does not cause the pointer to be
  // captured, even though the loaded value might be the pointer itself
  // (think of self-referential objects).
  if (Call->isCallee(&U))
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  // Check if that's the argument which can escape through this call
  // Not captured if only passed via 'nocapture' arguments.
  if (Call->isDataOperand(&U) &&
      !Call->doesNotCapture(Call->getDataOperandNo(&U)) &&
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
    const auto ArgEscReason = getArgEscStatus(ArgNo, Callee);
    if (ArgEscReason.any() && (ArgEscReason != EscReasonBits::PTR_ARG_ALIASING))
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::PASSING_TO_CALL};
  }
  return {EscKindTy::NO_ESCAPE, std::nullopt};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoLoad(const Instruction *I) {
  // LLVM_DEBUG(dbgs() << " -- Load\n");
  // Volatile loads make the address observable.
  if (cast<LoadInst>(I)->isVolatile())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::VOLATILE};
  return {EscKindTy::NO_ESCAPE, std::nullopt};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoStore(const Use &U, const Instruction *I) {
  // LLVM_DEBUG(dbgs() << " -- Store\n");
  // Volatile stores make the address observable.
  if (cast<StoreInst>(I)->isVolatile())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::VOLATILE};

  if (U.getOperandNo() != 0)
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  // Passing value instead of pointer is neither escape nor alias
  // if (const auto *Src = I->getOperand(0); !Src->getType()->isPointerTy()) {
    // dbgs() << "Passing value instead of pointer is neither escape nor alias\n";
    // return {EscKindTy::NO_ESCAPE, std::nullopt};
  // }

  const auto DstObjs = getUnderlyingMayEscObjs(I->getOperand(1));
  if (DstObjs.empty()) {
    // dbgs() << "No underlying objects for store\n";
    return {EscKindTy::NO_ESCAPE, std::nullopt};
  }

  return {EscKindTy::MAY_ALIASING, DstObjs};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoAtomicRMW(const Use &U,
                                        const Instruction *I) {
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
                                            const Instruction *I) {
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
EscapeAnalysisInfo::getEscInfoGetElementPtr(const Instruction *I) {
  // LLVM_DEBUG(dbgs() << " -- GetElementPtr\n");
  // AA does not support pointers of vectors, so GEP vector splats need to
  // be considered as captures.
  if (I->getType()->isVectorTy())
    return {EscKindTy::MAY_ESCAPE, EscReasonBits::OTHER};

  // GEP itself is not escape or alias
  return {EscKindTy::NO_ESCAPE, std::nullopt};
}

EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscInfoICmp(const Use &U, const Instruction *I) {
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
EscapeAnalysisInfo::getEscInfoRet(const Use &U) {
  // LLVM_DEBUG(dbgs() << " -- Ret\n");
  // If not return pointer, means that's not escape
  // Returning null pointer is not escape
  if (!U->getType()->isPointerTy() || (isa<ConstantPointerNull>(U.get())))
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  return {EscKindTy::MAY_ESCAPE, EscReasonBits::RET_PTR};
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
  default:
    // LLVM_DEBUG(dbgs() << " -- Default\n");
    return {EscKindTy::NO_ESCAPE, std::nullopt};
    // Need to recheck, maybe we behave too aggressive, because previous logic
    // was return {EscapeKind::MAY_ESCAPE, std::nullopt};
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
EscapeAnalysisInfo::findObjInBBEscapeState(const BasicBlock *BB,
                                           const Value *V) const {
  const auto It = BBEscapeStates.find(BB);
  assert((It != BBEscapeStates.end()) && "Cannot find BBEscapeState for BB\n");
  return It->second.getEscReason(V);
}

/// Is Value V is escaping in some path from Entry to BB?
bool EscapeAnalysisInfo::isEscapedForBB(const BasicBlock *BB, const Value *V,
                                        EscReasonTy *EscReason) const {
  const auto ExtStatus = getExtObjStatus(V);
  if (ExtStatus.any()) {
    if (EscReason)
      *EscReason = ExtStatus;
    return true;
  }

  const auto FoundStatus = findObjInBBEscapeState(BB, V);
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
bool EscapeAnalysisInfo::isEscapedForBBIPA(const BasicBlock *BB, const Value *V,
                                           EscReasonTy *EscReason) const {
  const auto ExtStatus = getExtObjStatusWithIPA(V);

  if (ExtStatus.any()) {
    if (EscReason)
      *EscReason = ExtStatus;
    return true;
  }

  const auto FoundStatus = findObjInBBEscapeState(BB, V);
  if (FoundStatus.any()) {
    if (EscReason)
      *EscReason = FoundStatus;
    return true;
  }

  if (EscReason)
    *EscReason = 0;
  return false;
}

/// Return escape reason for V in BB
EscapeAnalysisInfo::EscReasonTy EscapeAnalysisInfo::getFullEscapedForBBReason(
    const BasicBlock *BB, const Value *V) const {
  // return getExtObjStatusWithArgLookup(V) | findObjInBBEscapeState(BB, V);
  return getExtObjStatusWithIPA(V) | findObjInBBEscapeState(BB, V);
}

/// Is Value V is escaping somewhere in the function
bool EscapeAnalysisInfo::isEscapedForFunc(
    const Value *V,
    std::optional<std::reference_wrapper<EscReasonTy>> EscReason) const {
  EscReasonTy CombinedEscReason;
  for (const auto &BB : AnalyzedFunc) {
    if (pred_empty(&BB) && !BB.isEntryBlock())
      continue;
    CombinedEscReason |= getFullEscapedForBBReason(&BB, V);
  }

  if (EscReason.has_value())
    EscReason->get() = CombinedEscReason;

  return CombinedEscReason.any();
}

void EscapeAnalysisInfo::printEscapingForBB(const BasicBlock *BB,
                                            raw_ostream &OS) const {
  const auto It = BBEscapeStates.find(BB);
  if ((It == BBEscapeStates.end()) || (It->second.getEscapedObjs().empty()))
    return;

  bool PrintedEscapeHeader = false;
  for (const auto &V : It->second.getEscapedObjs()) {
    if (!PrintedEscapeHeader) {
      OS << "Escaping objects for BB " << BB->getName() << ":\n";
      PrintedEscapeHeader = true;
    }

    if (!isa<GlobalValue>(V.first)) {
      OS << *V.first << "\n";
      // LLVM_DEBUG(OS << "EscReason: "; printEscReason(V.second); );
    }
  }
  if (PrintedEscapeHeader)
    OS << "\n";
}

void EscapeAnalysisInfo::print(raw_ostream &OS) const {
  for (const auto &BB: AnalyzedFunc)
    printEscapingForBB(&BB, OS);
  // LLVM_DEBUG(if (IPAFuncEscInfo)
    // OS << "IsRetEscape: " << IsRetEscape << "\n\n");
}

AnalysisKey EscapeAnalysis::Key;

EscapeAnalysis::Result EscapeAnalysis::run(const Function &F,
                                           FunctionAnalysisManager &AM) {
  EscapeAnalysisInfo EAI(F);
  return EAI;
}

PreservedAnalyses
EscapeAnalysisPrinterPass::run(Function &F, FunctionAnalysisManager &AM) const {
  OS << "Printing analysis 'Escape Analysis' for function '"
      << F.getName() << "':\n";
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
    LLVM_DEBUG(dbgs() << "@@@@@@@@@ ArgEsc: " << Arg << " -- "
                      << EAI.isEscapedForFunc(&Arg) << "\n";);
    EAI.isEscapedForFunc(&Arg, std::ref(ArgEscReason));
    (*IPABottomTopEscInfo)[F].ArgEscapes[Arg.getArgNo()] = ArgEscReason;
  }
  // LLVM_DEBUG(dbgs() << "@@@@@@@@@ RetEsc: " << EAI.getIsRetEscape() << "\n");
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
  const auto It = IPATopDownArgEscInfo->try_emplace(F, F->arg_size(), false);
  auto &IsArgEscaped = It.first->second;

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
      if (F->isVarArg() &&
          (ArgIdx >= F->getFunctionType()->getNumParams()))
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

      const auto UnderlObjs =
          EscapeAnalysisInfo::getUnderlyingMayEscObjs(Arg);
      const auto EAIIt = FuncEscapeInfo.find(CB->getFunction());
      assert(EAIIt != FuncEscapeInfo.end());
      const EscapeAnalysisInfo &CallEAI = EAIIt->second;
      const auto *BB = CB->getParent();

      for (const auto &UnderlObj : UnderlObjs) {
        auto checkEscapeStatus = [&](const Value *Obj) {
          IsArgEscaped[ArgIdx] |=
              CallEAI.isEscapedForBBIPA(BB, Obj);
          LLVM_DEBUG(dbgs() << "\t\tArg " << ArgIdx << " (underl obj: " << *Obj
                            << ") is escaped: "
                            << (IsArgEscaped[ArgIdx] ? "YES" : "NO") << "\n");
          return IsArgEscaped[ArgIdx];
        };

        if (UnderlObj.Loaded)
          CallEAI.forEachPointeeDo(UnderlObj.Obj, BB, checkEscapeStatus);
        else
          checkEscapeStatus(UnderlObj.Obj);

        if (IsArgEscaped[ArgIdx])
          break;
      }
    }
  }
}

bool EscapeAnalysisGlobalInfo::traverseCGBottomTop(
    CallGraph &CG, const SmallPtrSetImpl<const Function *> &RecursiveFuncs,
    SmallVector<std::vector<CallGraphNode *> > &SCCList) {
  // Main callgraph traversal
  for (auto It = scc_begin(&CG); !It.isAtEnd(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;
    assert(!SCC.empty() && "SCC with no functions?");
    SCCList.push_back(SCC);

    bool Converged = false;
    while (!Converged) {
      Converged = true;
      for (const CallGraphNode *CGN : SCC) {
        const auto *F = CGN->getFunction();
        if (!F || F->isDeclaration())
          continue;

        // Build escape summary for a function
        FuncEscapeInfo.erase(F);

        const auto [It, Inserted] = FuncEscapeInfo.try_emplace(
            F, EscapeAnalysisInfo(*F, IPABottomTopEscInfo));

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

bool EscapeAnalysisGlobalInfo::isFuncPassedToObjCSelector(const Function *F) {
  for (const GlobalVariable &GV : M.globals()) {
    if (GV.getName().starts_with("OBJC_SELECTOR_REFERENCES_") && GV.
        hasInitializer()) {
      const auto *Initializer = GV.getInitializer();

      if (Initializer->getName() == "OBJC_METH_VAR_NAME_") {
        if (const auto *InitStr = dyn_cast<ConstantDataArray>(
            Initializer->getOperand(0))) {
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
      for (CallGraphNode *CGN : SCC) {
        const Function *F = CGN->getFunction();

        // We consider only localy defined, static functions
        if (!isLocalAndExactFunc(F) || !F->hasLocalLinkage() ||
            // For now, conservatively skip all ObjC methods
            isFuncPassedToObjCSelector(F))
          continue;

        LLVM_DEBUG(dbgs() << "\nFunc: " << F->getName() << "\n");

        // For non-recursive functions, no need to iterate until convergence
        if ((SCC.size() == 1) && !RecursiveFuncs.contains(F)) {
          evalTopDownArgEscStatus(FuncCallSites, F);
          FuncEscapeInfo.erase(F);
          FuncEscapeInfo.try_emplace(F,
                                     EscapeAnalysisInfo(*F, IPABottomTopEscInfo,
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
            F,
            EscapeAnalysisInfo(*F, IPABottomTopEscInfo, IPATopDownArgEscInfo));

        if (Converged && (PrevIPAFuncInfo != (*IPATopDownArgEscInfo)[F]))
          Converged = false;
      }
    }
  }
}

EscapeAnalysisGlobalInfo::EscapeAnalysisGlobalInfo(CallGraph &CG, Module &M_)
    : M(M_) {
  // We do a bottom-up SCC traversal of the call graph.  In other words, we
  // visit all callees before callers (leaf-first).

  // This is needed to (conservatively) consider recursive calls and SCCs.
  // First, find all SCCs and set all pointer argument as escaped
  SmallPtrSet<const Function *, 8> RecursiveFuncs;
  traverseSCCsAndInitIPAEscInfo(CG, RecursiveFuncs);

  LLVM_DEBUG(printIPAFuncEscInfo(IPABottomTopEscInfo.get(), dbgs()));

  // We need it to traverse call graph in reverse (top-bottom) order later
  SmallVector<std::vector<CallGraphNode *>> SCCList;

  traverseCGBottomTop(CG, RecursiveFuncs, SCCList);

  LLVM_DEBUG(dbgs() << "###################################################\n");
  LLVM_DEBUG(dbgs() << "######## IPA Bottom-Top Escape Analysis Done ##### \n");
  LLVM_DEBUG(dbgs() << "###################################################\n");

  // For each function, get list of instruction calling it
  const auto FuncCallSites = getFuncToCallSitesMap();

  // Compute how arguments escape from passing escaped parameters in calls
  traverseCGTopDown(SCCList, FuncCallSites, RecursiveFuncs);

  LLVM_DEBUG(printArgEscStatus(););
  LLVM_DEBUG(dbgs() << "\n");
}

/// For given pointer, get underlying objects, and get escape status for them
bool EscapeAnalysisGlobalInfo::isEscapedUndrlObjOrPointee(
    const Value *Addr, const BasicBlock *BB,
    EscapeAnalysisInfo::EscReasonTy &EscReason) {
  for (const UnderlObjInfo &UnderlObj :
       EscapeAnalysisInfo::getUnderlyingMayEscObjs(Addr)) {
    LLVM_DEBUG(dbgs() << "isEscapedUndrlObjOrPointee UnderlObj: "
                      << *UnderlObj.Obj << "\n");
    if (isEscapedForBBTSan(BB->getParent(), BB, UnderlObj, EscReason))
      return true;
  }
  return false;
}

bool EscapeAnalysisGlobalInfo::isEscapedForBBTSan(
    const Function *F, const BasicBlock *BB, const UnderlObjInfo &UnderlObj,
    EscapeAnalysisInfo::EscReasonTy &EscReason) {
  if (const auto FEIIt = FuncEscapeInfo.find(F);
      FEIIt != FuncEscapeInfo.end()) {
    // if (FEIIt->second.isEscapedForBB(BB, UnderlObj.Obj, &EscReason))
    // return true;
    if (EscapeAnalysisInfo::isNonConstGV(UnderlObj.Obj))
      return true;
    // if (isa<GlobalVariable>(UnderlObj.Obj))
      // return true;

    if (UnderlObj.Loaded) {
      bool IsEscaped = false;
      FEIIt->second.forEachPointeeDo(UnderlObj.Obj, BB, [&](const Value *V) {
        if (FEIIt->second.isEscapedForBBIPA(BB, V, &EscReason))
          IsEscaped = true;
      });
      return IsEscaped;
    }
    return FEIIt->second.isEscapedForBBIPA(BB, UnderlObj.Obj, &EscReason);
  }
  return true;
}

void EscapeAnalysisGlobalInfo::print(Module &M, raw_ostream &O) const {
  for (const Function &F: M) {
    if (!isLocalAndExactFunc(&F))
      continue;

    const auto It = FuncEscapeInfo.find(&F);
    assert(It != FuncEscapeInfo.end() && "Function EA results must exist\n");
    O << "Printing analysis 'Escape Analysis' for function '" << F.getName()
      << "':\n";
    It->second.print(O);
  }
}

AnalysisKey EscapeAnalysisGlobal::Key;

EscapeAnalysisGlobal::Result
EscapeAnalysisGlobal::run(Module &M, ModuleAnalysisManager &AM) {
  return EscapeAnalysisGlobalInfo(AM.getResult<CallGraphAnalysis>(M), M);
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

/// Wrapper around getUnderlyingObject to look through loads
static UnderlObjInfo getUnderlyingObjectThroughLoads(const Value *&P,
                                                     const unsigned MaxLookup) {
  bool LoadInstFlag = false;
  while (true) {
    P = getUnderlyingObject(P, MaxLookup);
    if (const auto *Load = dyn_cast<LoadInst>(P)) {
      P = Load->getPointerOperand();
      LoadInstFlag = true;
    } else {
      return {P, LoadInstFlag};
    }
  }
}

/// This method is similar to getUnderlyingObject except that it can
/// look through phi and select instructions and return multiple objects.
///
/// This is slightly modified version from ValueTracking.cpp. The differences:
/// 1. Pass through LoadInst to get the original loaded object.
/// 2. Ignore phi invariant check.
static void getUnderlyingObjectsWithoutPHIInvCheck(
    const Value *V, SmallVectorImpl<UnderlObjInfo> &Objects,
    const unsigned MaxLookup) {
  LLVM_DEBUG(dbgs() << "getUnderlyingObjectsWithoutPHIInvCheck: " << *V
                    << "\n");
  SmallPtrSet<const Value *, 4> Visited;
  // SmallVector<const Value *, 4> Worklist;
  SmallVector<UnderlObjInfo> Worklist;
  Worklist.push_back({V, false});
  do {
    const Value *Obj = Worklist.pop_back_val().Obj;
    auto P = getUnderlyingObjectThroughLoads(Obj, MaxLookup);

    if (!Visited.insert(P.Obj).second)
      continue;

    // TODO use & operator for LoadFlag to combine loads
    if (auto *SI = dyn_cast<SelectInst>(P.Obj)) {
      Worklist.push_back({SI->getTrueValue(), P.Loaded});
      Worklist.push_back({SI->getFalseValue(), P.Loaded});
      continue;
    }

    if (auto *PN = dyn_cast<PHINode>(P.Obj)) {
      LLVM_DEBUG(dbgs() << "PHI node: " << *PN << "\n");
      // In original function, we check here whether PHI is invariant during
      // the loop. In this version, we are conservative and ignore it.
      // append_range(Worklist, PN->incoming_values());
      for (const Value *Incoming : PN->incoming_values()) {
        LLVM_DEBUG(dbgs() << "Incoming: " << *Incoming << "\n");
        Worklist.push_back({Incoming, P.Loaded});
      }
      continue;
    }

    Objects.push_back(P);
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
static bool getUnderlyingObjectsForCodeGenWithoutPHIInvCheck(
    const Value *V, SmallVectorImpl<UnderlObjInfo> &Objects, const unsigned MaxLookup,
    std::shared_ptr<EscapeAnalysisInfo::IPABottomTopMap> IPAFuncEscInfo) {
  SmallPtrSet<const Value *, 16> Visited;
  SmallVector<const Value *, 4> Working(1, V);
  do {
    V = Working.pop_back_val();

    SmallVector<UnderlObjInfo> Objs;
    getUnderlyingObjectsWithoutPHIInvCheck(V, Objs, MaxLookup);

    for (const auto &VV : Objs) {
      if (!Visited.insert(VV.Obj).second)
        continue;
      if (Operator::getOpcode(VV.Obj) == Instruction::IntToPtr) {
        const Value *OWithoutCast =
            getUnderlyingObjectFromInt(cast<User>(VV.Obj)->getOperand(0));

        // Pass through loads
        const auto UnderlObj = getUnderlyingObjectThroughLoads(
            OWithoutCast, MaxLookup);

        if (UnderlObj.Obj->getType()->isPointerTy() ||
            isa<PHINode>(UnderlObj.Obj)) {
          Working.push_back(UnderlObj.Obj);
          continue;
        }
      }

      // If getUnderlyingObjects fails to find an identifiable object,
      // getUnderlyingObjectsForCodeGen also fails for safety.
      if (!isIdentifiedObject(VV.Obj) &&
          // Function arguments may escape or be aliases */
          !isa<Argument>(VV.Obj) &&
          // Results of function calls (e.g. returning pointer) may escape
          !isCallMayEscape(VV.Obj, IPAFuncEscInfo)) {
        Objects.clear();
        return false;
      }
      Objects.push_back(VV);
    }
  } while (!Working.empty());
  return true;
}

/// Recuresively search in the instruction for the underlying objects which
/// may escape
SmallVector<UnderlObjInfo> EscapeAnalysisInfo::getUnderlyingMayEscObjs(
    const Value *V, const unsigned MaxLookup,
    std::shared_ptr<IPABottomTopMap> IPAFuncEscInfo) {
  SmallVector<UnderlObjInfo> UnderlObjs;
  getUnderlyingObjectsForCodeGenWithoutPHIInvCheck(V, UnderlObjs, MaxLookup,
                                                   IPAFuncEscInfo);

  LLVM_DEBUG(dbgs() << "\tgetUnderlyingMayEscObjects for " << *V << "\n");
  LLVM_DEBUG(if (!UnderlObjs.empty()) {
  dbgs() << "\tgetUnderlyingMayEscObjects:";
  for (const auto &Obj : UnderlObjs) dbgs() << "\t\t" << *Obj.Obj << "\n"; }
  else dbgs() << "\tgetUnderlyingMayEscObjects -- empty\n"; );

  return UnderlObjs;
}
