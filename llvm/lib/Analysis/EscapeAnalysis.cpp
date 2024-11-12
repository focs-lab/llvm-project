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
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"

#include <deque>

using namespace llvm;

#define DEBUG_TYPE "ea"
#define DEEP_DEBUG_TYPE "ea-deep-debug"

cl::opt<std::string> PrintEscapeAnalysis(
    "print-escape-analysis", cl::Hidden,
    cl::desc("The option to specify the name of the function "
             "whose escape analysis result is printed."));

// STATISTIC(NumEscapedGPtr, "Number of escaped by assignment to global pointer");
STATISTIC(NumEscapedCall, "Number of escaped by passing to a function");
// STATISTIC(NumEscapedRet, "Number of escaped by passing to a function");

//===----------------------------------------------------------------------===//
// Alias relation
//===----------------------------------------------------------------------===//

/// Add the alias: Alias --> PointeeValue
void EscapeAnalysisInfo::EscapeState::addAlias(
    const Value *Alias, const Value *PointeeValue) {
  assert(Alias->getType()->isPointerTy() && "Alias must be a pointer\n");

  if ((Alias == PointeeValue) || (PointeeValue == nullptr) ||
      (AliasRel.AliasMap[Alias].contains(PointeeValue)))
    return;

  LLVM_DEBUG(dbgs() << "\taddAlias: " << *PointeeValue << " --> " << *Alias
                    << "\n");
  AliasRel.AliasMap[Alias].insert(PointeeValue);

  // If instruction creates an alias to the object which has escaped before
  // or escapes "by definition" (e.g. pointer function argument,
  // global pointer), then that's not just aliasing, but escaping as well
  if (isExternalEscapedObject(PointeeValue) ||
      EscapedObjects.contains(PointeeValue))
    addEscapingObject(Alias);

  // Considering transitivity: recursively add new alias to all existing aliases
  // of PointeeValue
  if (const auto ExistAliases = AliasRel.getAliases(PointeeValue); ExistAliases)
    for (const Value *ExistingAlias : ExistAliases.value())
      addAlias(Alias, ExistingAlias);

  // TODO: remove condition?
  if (isa<AllocaInst>(Alias) ||
      (isa<Argument>(Alias) && !Alias->getType()->isPointerTy()))
    addAlias(PointeeValue, Alias);
}

/// Get list of aliases for the object a
std::optional<EscapeAnalysisInfo::AliasRelationTy::AliasListTy>
EscapeAnalysisInfo::AliasRelationTy::getAliases(const Value *V) const {
  const auto It = AliasMap.find(V);
  if (It == AliasMap.end())
    return std::nullopt;
  return It->second;
}

/// We need it to check if something changed in the data-flow analysis
bool EscapeAnalysisInfo::AliasRelationTy::operator==(
    const AliasRelationTy &Other) const {
  if (this == &Other)
    return true;
  return AliasMap == Other.AliasMap;
}

/// Print alias relation
void EscapeAnalysisInfo::AliasRelationTy::print(raw_ostream &OS) const {
  OS << "Alias relations:\n";
  for (const auto &[Key, ValueSet] : AliasMap) {
    OS << "\tAlias: " << *Key << "\n";
    for (const Value *Alias : ValueSet)
      OS << "\t\t --> " << *Alias << "\n";
  }
  OS << "\n";
}

//===----------------------------------------------------------------------===//
// EscapeState
//===----------------------------------------------------------------------===//

bool EscapeAnalysisInfo::EscapeState::operator==(
    const llvm::EscapeAnalysisInfo::EscapeState &ES) const {
  if (this == &ES) return true;
  return ((EscapedObjects == ES.EscapedObjects) &&
          (AliasRel == ES.AliasRel));
}

void EscapeAnalysisInfo::EscapeState::addEscapingObject(
    const Value *EscapingObject) {
  SmallPtrSet<const Value *, 8> EscObjList;
  getEscapingObjectsList(EscapingObject, EscObjList);
  DEBUG_WITH_TYPE(DEEP_DEBUG_TYPE,
    for (auto *V: EscObjList)
      dbgs() << "Add escaping object: " << *V << "\n";
    dbgs() << "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";);

  EscapedObjects.insert(EscObjList.begin(), EscObjList.end());
}

void EscapeAnalysisInfo::EscapeState::getEscapingObjectsList(
    const Value *EscapingObject, SmallPtrSetImpl<const Value *> &EscObjList) {
  if (EscObjList.contains(EscapingObject))
    return;

  EscObjList.insert(EscapingObject);

  // Suppose we add as escaping an object which is alias of some other objects.
  // Then all these aliases also escape!
  if (const auto Aliases = AliasRel.getAliases(EscapingObject); Aliases.has_value())
    for (const Value *Alias : Aliases.value())
      getEscapingObjectsList(Alias, EscObjList);
}

//===----------------------------------------------------------------------===//
// Main analysis
//===----------------------------------------------------------------------===//

EscapeAnalysisInfo::EscapeAnalysisInfo(const Function &Fn): F(Fn) {
  std::deque<const BasicBlock *> WorkList;

  // Try to traverse CFG in reverse post-order
  ReversePostOrderTraversal<const Function *> RPOT(&F);
  for (const BasicBlock *BB : RPOT) {
    WorkList.push_back(BB);
    // BBEscapeStates[BB] = EscapeState(BB);
    BBEscapeStates[BB] = EscapeState();
  }

  while (!WorkList.empty()) {
    const BasicBlock *BB = WorkList.front();
    WorkList.pop_front();

    LLVM_DEBUG(dbgs() << "****************** BB " << BB->getName() << " (func "
                      << Fn.getName() << ") ******************\n");

    EscapeState NewES = mergePredEscapeStates(BB);
    compOutEscapeState(BB, NewES);

    // If something changed, proceed with this BB
    LLVM_DEBUG(dbgs() << "\n>> Check changes for " << BB->getName() << " -- ");
    if (NewES != BBEscapeStates[BB]) {
      LLVM_DEBUG(dbgs() << "Changed!\n\n");
      // Add BB's successors to WorkList and update BB state
      for (const auto *SuccBB : successors(BB)) {
        LLVM_DEBUG(dbgs() << "Add succ BB " << SuccBB->getName() << "\n");
        WorkList.push_back(SuccBB);
      }
      BBEscapeStates[BB] = NewES;
    } else {
      LLVM_DEBUG(dbgs() << "Not Changed!\n\n");
    }

    LLVM_DEBUG(
      printEscapingForBB(BB, dbgs());
      BBEscapeStates[BB].AliasRel.print(dbgs());
      dbgs() << "******** END of BB " <<  BB->getName() << " ******** \n\n";
    );
  }
}

/// Compute the resulting escape state for BB
void EscapeAnalysisInfo::compOutEscapeState(const BasicBlock *BB,
                                            EscapeState &ES) {
  for (const Instruction &I : *BB) {
    LLVM_DEBUG(dbgs() << "\nI " << I << "\n");
    for (const Use &Opnd : I.operands()) {
      LLVM_DEBUG(dbgs() << "\n\tOPND \t" << *Opnd.get() << "\n";);

      const auto [EscKind, Aliases] = getEscapeKindForOpnd(Opnd);

      if (EscKind == EscapeKind::NO_ESCAPE)
        continue;

      const auto UnderlyingObjs = getUnderlyingMayEscObjects(Opnd.get());
      if (UnderlyingObjs.empty())
        continue;

      if (EscKind == EscapeKind::MAY_ESCAPE) {
        LLVM_DEBUG(dbgs() << "\t-- MAY_ESCAPE --\n");
        for (const Value *EO : UnderlyingObjs)
          ES.addEscapingObject(EO);
      } else {
        assert(EscKind == EscapeKind::MAY_ALIASING);
        assert(Aliases != std::nullopt && !Aliases.value().empty() &&
               "If found alias, alias must be set\n");

        LLVM_DEBUG(dbgs() << "\t-- ALIASING --\n";
                   for (auto *A : Aliases.value())
                     dbgs() << "\tAlias candidate: " << *A << "\n");

        for (const Value *Alias : Aliases.value())
          for (const Value *Pointee : UnderlyingObjs)
            ES.addAlias(Alias, Pointee);
      }
    }
  }
}

EscapeAnalysisInfo::EscapeState EscapeAnalysisInfo::mergePredEscapeStates(
    const BasicBlock *BB) {
  // EscapeState MergedES(BB);
  EscapeState MergedES;

  // Merge states of predecessors
  for (auto *PredBB : predecessors(BB)) {
    LLVM_DEBUG(dbgs() << "Merge to << " << BB->getName() << " <-- "
                      << PredBB->getName() << "\n");
    auto &PredES = BBEscapeStates[PredBB];
    MergedES.merge(PredES);
  }
  return MergedES;
}

/// Check whether type contains pointers
bool EscapeAnalysisInfo::containsPointerType(const Type *Ty) {
  if (Ty->isPointerTy()) return true;
  if (!Ty->isStructTy()) return false;

  for (Type *EltTy : Ty->subtypes())
    if (containsPointerType(EltTy))
      return true;
  return false;
}

/// Escaping state for the function is the escape state for Exit BB
const EscapeAnalysisInfo::EscapedObjectsTy &
EscapeAnalysisInfo::getFuncEscState() const {
  const auto It = BBEscapeStates.find(&F.back());
  assert(It != BBEscapeStates.end() &&
         "Escape state for exit  block  not  found");
  return It->second.EscapedObjects;
}

/// Determine what kind of escape behaviour V may exhibit.
std::pair<EscapeAnalysisInfo::EscapeKind,
          std::optional<SmallVector<Value *, 8>>>
EscapeAnalysisInfo::getEscapeKindForOpnd(const Use &U) {
  LLVM_DEBUG(dbgs() << "\tgetEscapeKindForPtrOpnd -- ");
  const auto *I = dyn_cast<Instruction>(U.getUser());
  if (!I)
    return {EscapeKind::NO_ESCAPE, std::nullopt};

  switch (I->getOpcode()) {
  case Instruction::Call:
  case Instruction::Invoke: {
    LLVM_DEBUG(dbgs() << "Call/Invoke\n");
    auto *Call = cast<CallBase>(I);

    // Not captured if the callee is readonly, doesn't return a copy through
    // its return value and doesn't unwind (a readonly function can leak bits
    // by throwing an exception or not depending on the input value).
    if (Call->onlyReadsMemory() && Call->doesNotThrow() &&
        Call->getType()->isVoidTy())
      return {EscapeKind::NO_ESCAPE, std::nullopt};

    // The pointer is not captured if returned pointer is not captured.
    // NOTE: CaptureTracking users should not assume that only functions
    // marked with nocapture do not capture. This means that places like
    // getUnderlyingObject in ValueTracking or DecomposeGEPExpression
    // in BasicAA also need to know about this property.
    if (isIntrinsicReturningPointerAliasingArgumentWithoutCapturing(Call, true))
      return {EscapeKind::MAY_ALIASING, getUnderlyingMayEscObjects(I)};

    // Volatile operations effectively capture the memory location that they
    // load and store to.
    if (auto *MI = dyn_cast<MemIntrinsic>(Call)) {
      if (MI->isVolatile())
        return {EscapeKind::MAY_ESCAPE, std::nullopt};

      const auto *Src = MI->getArgOperand(1);
      const auto DstObjs = getUnderlyingMayEscObjects(MI->getArgOperand(0));

      // Considering llvm.memcpy intrinsic
      if ((MI->getIntrinsicID() == Intrinsic::memcpy) && (Src == U.get())) {
        // Check whether the source argument is a struct containing pointers
        if (const auto *Alloca = dyn_cast<AllocaInst>(U.get())) {
          const Type *StructTy = Alloca->getAllocatedType();
          if (StructTy && containsPointerType(StructTy))
            return {EscapeKind::MAY_ALIASING, DstObjs};
        }
      }
    }

    // Calling a function pointer does not in itself cause the pointer to
    // be captured.  This is a subtle point considering that (for example)
    // the callee might return its own address.  It is analogous to saying
    // that loading a value from a pointer does not cause the pointer to be
    // captured, even though the loaded value might be the pointer itself
    // (think of self-referential objects).
    if (Call->isCallee(&U))
      return {EscapeKind::NO_ESCAPE, std::nullopt};

    // Not captured if only passed via 'nocapture' arguments.
    if (Call->isDataOperand(&U) &&
        !Call->doesNotCapture(Call->getDataOperandNo(&U)) &&
        U->getType()->isPointerTy()) {
      // The parameter is passed by pointer and not marked 'nocapture'.
      ++NumEscapedCall;
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    }
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::Load:
    LLVM_DEBUG(dbgs() << "Load\n");
    // Volatile loads make the address observable.
    if (cast<LoadInst>(I)->isVolatile())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  case Instruction::Store: {
    LLVM_DEBUG(dbgs() << "Store\n");
    // Volatile stores make the address observable.
    if (cast<StoreInst>(I)->isVolatile())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};

    if (U.getOperandNo() != 0)
      return {EscapeKind::NO_ESCAPE, std::nullopt};

    const auto *Src = I->getOperand(0);

    // Passing value instead of pointer is neither escape nor alias
    if (!Src->getType()->isPointerTy())
      return {EscapeKind::NO_ESCAPE, std::nullopt};

    const auto DstObjs = getUnderlyingMayEscObjects(I->getOperand(1));
    if (DstObjs.empty())
      return {EscapeKind::NO_ESCAPE, std::nullopt};

    // Store to GV - escape
    for (const auto *Obj : DstObjs)
      if (isa<GlobalVariable>(Obj))
        return {EscapeKind::MAY_ESCAPE, std::nullopt};

    return {EscapeKind::MAY_ALIASING, DstObjs};
  }
  case Instruction::AtomicRMW: {
    LLVM_DEBUG(dbgs() << "AtomicRMW\n");
    // atomicrmw conceptually includes both a load and store from
    // the same location.
    // As with a store, the location being accessed is not captured,
    // but the value being stored is.
    // Volatile stores make the address observable.
    const auto *ARMWI = cast<AtomicRMWInst>(I);
    if (U.getOperandNo() == 1 || ARMWI->isVolatile())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::AtomicCmpXchg: {
    LLVM_DEBUG(dbgs() << "AtomicCmpXchg\n");
    // cmpxchg conceptually includes both a load and store from
    // the same location.
    // As with a store, the location being accessed is not captured,
    // but the value being stored is.
    // Volatile stores make the address observable.
    if (const auto *ACXI = cast<AtomicCmpXchgInst>(I);
        U.getOperandNo() == 1 || U.getOperandNo() == 2 || ACXI->isVolatile())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::GetElementPtr: {
    LLVM_DEBUG(dbgs() << "GetElementPtr\n");
    // AA does not support pointers of vectors, so GEP vector splats need to
    // be considered as captures.
    if (I->getType()->isVectorTy())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};

    // GEP itself is not escape or alias
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::ICmp: {
    LLVM_DEBUG(dbgs() << "ICmp\n");
    const unsigned Idx = U.getOperandNo();
    const unsigned OtherIdx = 1 - Idx;
    if (auto *CPN = dyn_cast<ConstantPointerNull>(I->getOperand(OtherIdx))) {
      // Don't count comparisons of a no-alias return value against null as
      // captures. This allows us to ignore comparisons of malloc results
      // with null, for example.
      if (CPN->getType()->getAddressSpace() == 0)
        if (isNoAliasCall(U.get()->stripPointerCasts()))
          return {EscapeKind::NO_ESCAPE, std::nullopt};

      if (!I->getFunction()->nullPointerIsDefined()) {
        auto *O = I->getOperand(Idx)->stripPointerCastsSameRepresentation();
        // Comparing a dereferenceable_or_null pointer against null cannot
        // lead to pointer escapes, because if it is not null it must be a
        // valid (in-bounds) pointer.
        const DataLayout &DL = I->getModule()->getDataLayout();
        if (isDereferenceableOrNull(O, DL))
          return {EscapeKind::NO_ESCAPE, std::nullopt};
      }
    }

    // Otherwise, be conservative. There are crazy ways to capture pointers
    // using comparisons.
    // return {CaptureKind::MAY_CAPTURE, std::nullopt};
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }

  case Instruction::Ret: {
    LLVM_DEBUG(dbgs() << "Ret\n");

    if (!U->getType()->isPointerTy())
      return {EscapeKind::NO_ESCAPE, std::nullopt};
    return {EscapeKind::MAY_ESCAPE, std::nullopt};
  }
  default:
    LLVM_DEBUG(dbgs() << "Default\n");
    // Something else - be conservative and say it is escaped.
    return {EscapeKind::NO_ESCAPE, std::nullopt};
    // return {EscapeKind::MAY_ESCAPE, std::nullopt};
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

//===----------------------------------------------------------------------===//
// getUnderlyingObject infrastracture (taken and modified from ValueTracker.cpp)
//===----------------------------------------------------------------------===//

/// Wrapper around getUnderlyingObject to look through loads
static const Value *getUnderlyingObjectThroughLoads(const Value *&P,
                                                    unsigned MaxLookup) {
  while (true) {
    P = getUnderlyingObject(P, MaxLookup);
    if (const auto *Load = dyn_cast<LoadInst>(P))
      P = Load->getPointerOperand();
    else
      return P;
  }
}

/// This method is similar to getUnderlyingObject except that it can
/// look through phi and select instructions and return multiple objects.
///
/// This is slightly modified version from ValueTracking.cpp. The differences:
/// 1. Look through LoadInst
/// 2. Ignore phi invariant check.
static void getUnderlyingObjectsWithoutPHIInvCheck(
    const Value *V, SmallVectorImpl<const Value *> &Objects,
    unsigned MaxLookup) {
  SmallPtrSet<const Value *, 4> Visited;
  SmallVector<const Value *, 4> Worklist;
  Worklist.push_back(V);
  do {
    const Value *P = Worklist.pop_back_val();

    P = getUnderlyingObjectThroughLoads(P, MaxLookup);

    if (!Visited.insert(P).second)
      continue;

    if (auto *SI = dyn_cast<SelectInst>(P)) {
      Worklist.push_back(SI->getTrueValue());
      Worklist.push_back(SI->getFalseValue());
      continue;
    }

    if (auto *PN = dyn_cast<PHINode>(P)) {
      // In original function, we check here whether PHI is invariant during
      // the loop. In this version, we are conservative and ignore it.
      append_range(Worklist, PN->incoming_values());
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
    const Value *V, SmallVectorImpl<Value *> &Objects,
    unsigned MaxLookup) {
  SmallPtrSet<const Value *, 16> Visited;
  SmallVector<const Value *, 4> Working(1, V);
  do {
    V = Working.pop_back_val();

    SmallVector<const Value *, 4> Objs;
    getUnderlyingObjectsWithoutPHIInvCheck(V, Objs, MaxLookup);

    LLVM_DEBUG(dbgs() << "\tgetUnderlyingObjectsWithoutPHIInvCheck:\n");
    for (const Value *VV : Objs) {
      LLVM_DEBUG(dbgs() << "\t\t" << *VV << "\n");
      if (!Visited.insert(VV).second)
        continue;
      if (Operator::getOpcode(VV) == Instruction::IntToPtr) {
        const Value *OWithoutCast =
            getUnderlyingObjectFromInt(cast<User>(VV)->getOperand(0));
        // Pass through loads
        const Value *O =
            getUnderlyingObjectThroughLoads(OWithoutCast, MaxLookup);
        if (O->getType()->isPointerTy()) {
          Working.push_back(O);
          continue;
        }
      }
      // If getUnderlyingObjects fails to find an identifiable object,
      // getUnderlyingObjectsForCodeGen also fails for safety.
      if (!isIdentifiedObject(VV) &&
          // Added because function arguments may escape or be aliases */
          !isa<Argument>(VV)) {
        Objects.clear();
        return false;
      }
      Objects.push_back(const_cast<Value *>(VV));
    }
  } while (!Working.empty());
  return true;
}

/// Recuresively search in the instruction for the underlying objects which
/// may escape
SmallVector<Value *, 8>
EscapeAnalysisInfo::getUnderlyingMayEscObjects(const Value *V,
                                               unsigned MaxLookup) {
  SmallVector<Value *, 8> UnderlyinglObjects;
  getUnderlyingObjectsForCodeGenWithoutPHIInvCheck(V, UnderlyinglObjects,
                                                   GetUndrlObjMaxLookup);
  LLVM_DEBUG(if (!UnderlyinglObjects.empty()) {
    dbgs() << "\tMayEscapeObjects (new):\n";
    for (auto *Obj : UnderlyinglObjects)
      dbgs() << "\t\t" << *Obj << "\n";
  });
  return UnderlyinglObjects;
}

/// Is Value V is escaping in some path from Entry to BB?
bool EscapeAnalysisInfo::isEscapingForBB(const BasicBlock *BB,
                                         const Value *V) const {
  if (isExternalEscapedObject(V))
    return true;

  const auto FoundIt = BBEscapeStates.find(BB);
  assert((FoundIt != BBEscapeStates.end()) &&
         "BBEscapeState must exist for each BB\n");
  LLVM_DEBUG(dbgs() << "isEscapedForBB: BB: " << BB->getName() << " V: " << *V
                    << " -- " << FoundIt->second.EscapedObjects.contains(V)
                    << "\n");
  return FoundIt->second.EscapedObjects.contains(V);
}

void EscapeAnalysisInfo::printEscapingForBB(const BasicBlock *BB,
                                            raw_ostream &OS) {
  const auto It = BBEscapeStates.find(BB);
  if ((It == BBEscapeStates.end()) || (It->second.EscapedObjects.empty()))
    return;

  OS << "Escaping objects for BB " << BB->getName() << ":\n";
  for (const auto *V : It->second.EscapedObjects) {
    if (isExternalEscapedObject(V))
      // I'm not sure, we should not print objects escaping by definition
      // (such as global variables or pointer arguments),
      // but let's omit them for now
      continue;
    OS << *V << "\n";
  }
  OS << "\n";
}

void EscapeAnalysisInfo::print(raw_ostream &OS) {
  ////
  // This is function-wise output
  //
  // const auto FuncEscapingAllocas = BBEscapeStates[&F.back()].EscapedObjects;
  // if (FuncEscapingAllocas.empty())
  //   return;
  // OS << "Escaping variables:\n";
  // for (const auto *V : FuncEscapingAllocas)
  //   OS << *V << "\n";
  // OS << "\n";

  for (const auto &BB: F)
    printEscapingForBB(&BB, OS);
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
