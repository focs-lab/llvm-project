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
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"

#include <deque>

using namespace llvm;

#define DEBUG_TYPE "ea"
#define DEEP_DEBUG_TYPE "ea-deep-debug"

// static cl::opt<std::string> PrintEscapeAnalysis(
//   "print-escape-analysis", cl::Hidden,
//   cl::desc("Print escape analysis info for all functions "
//            "using intraprocedural analysis."));
//
// static cl::opt<std::string> PrintEscapeAnalysisGlobal(
//     "print-escape-analysis-global", cl::Hidden,
//     cl::desc("Print global escape analysis info for the module."));

// STATISTIC(NumEscapedGPtr, "Number of escaped by assignment to global pointer");
// STATISTIC(NumEscapedCall, "Number of escaped by passing to a function");
// STATISTIC(NumEscapedRet, "Number of escaped by passing to a function");

//===----------------------------------------------------------------------===//
// Alias relation
//===----------------------------------------------------------------------===//

/// Add the alias: Alias --> PointeeValue
void EscapeAnalysisInfo::EscapeState::addAlias(const Value *Alias,
                                               const Value *PointeeValue,
                                               const EscapeAnalysisInfo *EAI) {
  assert(Alias->getType()->isPointerTy() && "Alias must be a pointer\n");

  if ((Alias == PointeeValue) || (PointeeValue == nullptr) ||
      AliasRel.AliasMap[Alias].contains(PointeeValue))
    return;

  LLVM_DEBUG(dbgs() << "\taddAlias: " << *PointeeValue << " --> " << *Alias
                    << "\n");
  AliasRel.AliasMap[Alias].insert(PointeeValue);

  // If instruction creates an alias to the object which has escaped before
  // or escapes "by definition" (e.g. pointer function argument,
  // global pointer), then that's not just aliasing, but escaping as well
  if (const auto EscReason = EAI->isExternalEscapedObject(PointeeValue);
      EscReason.any())
    // addEscapingObject(Alias, EscReason);
    addEscapeObjOrReason(Alias, EscReason);

  // If pointee object is escaped (as observed from previous analysis)
  if (const auto It = EscapedObjects.find(PointeeValue);
      It != EscapedObjects.end())
    // addEscapingObject(Alias, It->second);
      addEscapeObjOrReason(Alias, It->second);

  // Considering transitivity: recursively add new alias to all existing aliases
  // of PointeeValue
  if (const auto ExistAliases = AliasRel.getAliases(PointeeValue); ExistAliases)
    for (const Value *ExistingAlias : ExistAliases.value())
      addAlias(Alias, ExistingAlias, EAI);

  // This is alias symmetry part: if Alias --> Pointee, then Pointee --> Alias
  // Need to recheck it.
  if (isa<AllocaInst>(Alias) ||
      (isa<Argument>(Alias) && !Alias->getType()->isPointerTy()))
    addAlias(PointeeValue, Alias, EAI);
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

bool EscapeAnalysisInfo::EscapeState::operator==(const EscapeState &ES) const {
  if (this == &ES)
    return true;
  return ((EscapedObjects == ES.EscapedObjects) && (AliasRel == ES.AliasRel));
}

void EscapeAnalysisInfo::EscapeState::addEscapeObjOrReason(
    const Value *EscObj, const EscReasonTy EscReason) {
  if (const auto EscObjIt = EscapedObjects.find(EscObj);
      EscObjIt != EscapedObjects.end()) {
    // Object is already escaped - add the escape reason
    EscObjIt->second |= EscReason;
  } else {
    // Object has not escaped before - add it
    EscapedObjects.insert({EscObj, EscReason});
  }
}

// void EscapeAnalysisInfo::EscapeState::addEscapingObject(
    // const Value *EscapingObject, const EscReasonTy EscReason) {
  // SmallPtrSet<const Value *, 8> Aliases;
  // getAliasSubtreeAsList(EscapingObject, Aliases);
  // DEBUG_WITH_TYPE(DEEP_DEBUG_TYPE,
    // for (auto *V: Aliases)
      // dbgs() << "Add escaping object: " << *V << "\n";
    // dbgs() << "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n";);

  // for (auto It = Aliases.begin(); It != Aliases.end(); ++It)
    // addEscapeObjOrReason(*It, EscReason);
// }

void EscapeAnalysisInfo::EscapeState::addEscapingObject(
    const Value *EscapingObject, const EscReasonTy EscReason) {
  SmallVector<const Value *, 8> WorkList;
  SmallPtrSet<const Value *, 8> Visited;

  WorkList.push_back(EscapingObject);

  while (!WorkList.empty()) {
    const Value *Current = WorkList.pop_back_val();

    if (!Visited.insert(Current).second)
      continue;

    addEscapeObjOrReason(Current, EscReason);

    if (const auto Aliases = AliasRel.getAliases(Current);
        Aliases.has_value())
      for (const Value *Alias : Aliases.value())
        if (!Visited.contains(Alias))
          WorkList.push_back(Alias);
  }
}


// void EscapeAnalysisInfo::EscapeState::getAliasSubtreeAsList(
//     const Value *Obj, SmallPtrSetImpl<const Value *> &AliasList) {
//   if (AliasList.contains(Obj))
//     return;
//
//   AliasList.insert(Obj);
//
//   // Suppose we add as escaping an object which is alias of some other objects.
//   // Then all these aliases also escape!
//   if (const auto Aliases = AliasRel.getAliases(Obj); Aliases.has_value())
//     for (const Value *Alias : Aliases.value())
//       getAliasSubtreeAsList(Alias, AliasList);
// }

// Try refactored version:
// SmallPtrSetImpl<const Value *> EscapeAnalysisInfo::EscapeState::getAliasSubtreeAsList(const Value *Obj) {
//   SmallPtrSet<const Value *, 8> AliasList;
//   SmallVector<const Value *, 8> WorkList;
//
//   WorkList.push_back(Obj);
//
//   while (!WorkList.empty()) {
//     const Value *Current = WorkList.pop_back_val();
//
//     if (!AliasList.insert(Current).second) {
//       continue;
//     }
//
//     if (const auto Aliases = AliasRel.getAliases(Current); Aliases.has_value()) {
//       for (const Value *Alias : Aliases.value()) {
//         if (!AliasList.contains(Alias)) {
//           WorkList.push_back(Alias);
//         }
//       }
//     }
//   }
//
//   return AliasList;
// }

void EscapeAnalysisInfo::EscapeState::mergeAliases(
    const EscapeState &OtherES, const EscapeAnalysisInfo *EAI) {
  for (const auto &[OtherKey, OtherValueSet] : OtherES.AliasRel.AliasMap)
    for (const auto *OtherPointeeValue : OtherValueSet)
      addAlias(OtherKey, OtherPointeeValue, EAI);
}

void EscapeAnalysisInfo::EscapeState::mergeEscapedObjects(
    const EscapeState &OtherES) {
  // Here we don't need to look through aliases, because if some object
  // has been added to EscapedObjects, then all it's aliases
  // have been added too
  for (const auto &[Obj, EscReason] : OtherES.EscapedObjects) {
    if (auto It = EscapedObjects.find(Obj); It != EscapedObjects.end())
      // Object exists, merge EscReason masks
      It->second |= EscReason;
    else
      // Object not exists, add it to the list with a reason
      EscapedObjects.insert({Obj, EscReason});
  }
}

EscapeAnalysisInfo::EscReasonTy
EscapeAnalysisInfo::isExternalEscapedObject(const Value *V) const {
  if (const auto *CI = dyn_cast<CallInst>(V);
    CI && CI->getFunctionType()->getReturnType()->isPointerTy()) {
    return EscReasonBits::PASSING_TO_CALL;
  }

  if (isa<GlobalVariable>(V))
    return EscReasonBits::GPTR_ALIASING;

  return isa<Argument>(V) && V->getType()->isPointerTy()
             ? EscReasonBits::PTR_ARG_ALIASING
             : 0;
}

//===----------------------------------------------------------------------===//
// Main analysis
//===----------------------------------------------------------------------===//

EscapeAnalysisInfo::EscapeAnalysisInfo(
    const Function &Fn,
    std::optional<std::reference_wrapper<ArgumentEscapesMap>> ArgsEsc)
    : AnalyzedFunc(Fn), ArgsEscapes(ArgsEsc) {
  LLVM_DEBUG(dbgs() <<
    "\n||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||\n"
    "|||||||||||||||||||| Func " << Fn.getName() << "\t||||||||||||||||||||||\n"
    "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||\n");
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
        LLVM_DEBUG(dbgs() << "Add succ BB " << SuccBB->getName() << "\n");
        WorkList.push_back(SuccBB);
      }
      BBEscapeStates[BB] = NewES;
    } else {
      LLVM_DEBUG(dbgs() << "Not Changed!\n\n");
    }

    LLVM_DEBUG(printEscapingForBB(BB, dbgs());
      BBEscapeStates[BB].getAliases().print(dbgs());
      dbgs() << "******** END of BB " <<  BB->getName() << " ******** \n\n";);
  }
}

/// Compute the resulting escape state for BB
void EscapeAnalysisInfo::compBBEscapeState(const BasicBlock *BB,
                                           EscapeState &ES) {
  for (const Instruction &I : *BB) {
    LLVM_DEBUG(dbgs() << "\nI " << I << "\n");
    for (const Use &Opnd : I.operands()) {
      LLVM_DEBUG(dbgs() << "\n\tOPND \t";
        if (const auto F = dyn_cast<Function>(Opnd.get()))
          dbgs() << F->getName() << "\t";
        else
          dbgs() << *Opnd.get() << "\t";);

      const auto [EscKind, EscDetails] = getEscapeKindForOpnd(Opnd);

      if (EscKind == EscKindTy::NO_ESCAPE)
        continue;

      assert(EscDetails.has_value() && "EscDetails must be set");

      const auto UnderlyingObjs = getUnderlyingMayEscObjects(Opnd.get());
      if (UnderlyingObjs.empty())
        continue;

      if (EscKind == EscKindTy::MAY_ESCAPE) {
        LLVM_DEBUG(dbgs() << "\t-- MAY_ESCAPE --\n");
        assert(std::holds_alternative<EscReasonTy>(EscDetails.value()) &&
          "getEscapeKindForOpnd must return escape reason");
        const auto EscReason = std::get<EscReasonTy>(EscDetails.value());

        // If that's return instruction, we should check if it can return
        // a pointer to some external object
        if ((I.getOpcode() == Instruction::Ret) && (!IsRetEscape))
          for (const Value *EO: UnderlyingObjs)
            if (isEscapedForFunc(EO))
              IsRetEscape = true;

        for (const Value *EO : UnderlyingObjs) {
          LLVM_DEBUG(dbgs() << "\t\taddEscapingObject: " << *EO << "\n");
          ES.addEscapingObject(EO, EscReason);
        }
      } else {
        assert(EscKind == EscKindTy::MAY_ALIASING);
        assert((std::holds_alternative<SmallVector<Value *, 8>>(
          EscDetails.value()) &&
          "getEscapeKindForOpnd must return alias list"));
        const auto AliasList = std::get<SmallVector<Value *, 8> >(
            EscDetails.value());

        LLVM_DEBUG(dbgs() << "\t-- ALIASING --\n";
                   for (const auto *A : AliasList)
                     dbgs() << "\tAlias candidate: " << *A << "\n");

        for (const Value *Alias : AliasList)
          for (const Value *Pointee : UnderlyingObjs)
            ES.addAlias(Alias, Pointee, this);
      }
    }
  }
}

EscapeAnalysisInfo::EscapeState EscapeAnalysisInfo::mergePredEscapeStates(
    const BasicBlock *BB) {
  EscapeState MergedES;
  // Merge states of predecessors
  for (auto *PredBB : predecessors(BB)) {
    LLVM_DEBUG(dbgs() << "Merge to << " << BB->getName() << " <-- "
                      << PredBB->getName() << "\n");
    const auto &PredES = BBEscapeStates[PredBB];
    MergedES.merge(PredES, this);
  }
  return MergedES;
}

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
const EscapeAnalysisInfo::EscapedObjectsTy &
EscapeAnalysisInfo::getFuncEscState() const {
  const auto It = BBEscapeStates.find(&AnalyzedFunc.back());
  assert(It != BBEscapeStates.end() &&
         "Escape state for exit  block  not  found");
  return It->second.getEscapedObjs();
}

/// Determine what kind of escape behaviour V may exhibit, return
/// escape reason and list of aliases if applicable.
EscapeAnalysisInfo::EscInfoTy
EscapeAnalysisInfo::getEscapeKindForOpnd(const Use &U) const {
  const auto *I = dyn_cast<Instruction>(U.getUser());
  if (!I)
    return {EscKindTy::NO_ESCAPE, std::nullopt};

  switch (I->getOpcode()) {
  case Instruction::Call:
  case Instruction::Invoke: {
    LLVM_DEBUG(dbgs() << " -- Call/Invoke\n");
    // This object is already escaped since it's external
    const auto EscReason = isExternalEscapedObject(U.get());
    if (EscReason.any() && EscReason != PTR_ARG_ALIASING)
      return {EscKindTy::NO_ESCAPE, std::nullopt};

    const auto *Call = cast<CallBase>(I);

    // Not captured if the callee is readonly, doesn't return a copy through
    // its return value and doesn't unwind (a readonly function can leak bits
    // by throwing an exception or not depending on the input value).
    if (Call->onlyReadsMemory() && Call->doesNotThrow() &&
        Call->getType()->isVoidTy())
      return {EscKindTy::NO_ESCAPE, std::nullopt};

    // The pointer is not captured if returned pointer is not captured.
    // NOTE: CaptureTracking users should not assume that only functions
    // marked with nocapture do not capture. This means that places like
    // getUnderlyingObject in ValueTracking or DecomposeGEPExpression
    // in BasicAA also need to know about this property.
    if (isIntrinsicReturningPointerAliasingArgumentWithoutCapturing(Call, true))
      return {EscKindTy::MAY_ALIASING, getUnderlyingMayEscObjects(I)};

    // Volatile operations effectively capture the memory location that they
    // load and store to.
    if (const auto *MI = dyn_cast<MemIntrinsic>(Call)) {
      if (MI->isVolatile())
        return {EscKindTy::MAY_ESCAPE, EscReasonTy(EscReasonBits::VOLATILE)};

      const auto *Src = MI->getArgOperand(1);

      // Considering llvm.memcpy intrinsic
      if ((MI->getIntrinsicID() == Intrinsic::memcpy) && (Src == U.get())) {
        // Check whether the source argument is a struct containing pointers
        if (const auto *Alloca = dyn_cast<AllocaInst>(U.get())) {
          if (const Type *StructTy = Alloca->getAllocatedType();
              StructTy && structContainsPointerType(StructTy)) {
            const auto DstObjs = getUnderlyingMayEscObjects(
                MI->getArgOperand(0));
            return {EscKindTy::MAY_ALIASING, DstObjs};
          }
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
      return {EscKindTy::NO_ESCAPE, std::nullopt};

    // Check if that's the argument which can escape through this call
    // Not captured if only passed via 'nocapture' arguments.
    if (Call->isDataOperand(&U) &&
        !Call->doesNotCapture(Call->getDataOperandNo(&U)) &&
        U->getType()->isPointerTy()) {
      // If that's IPA, passing to calls is not escape
      if (ArgsEscapes.has_value()) {
        const Function *Callee = Call->getCalledFunction();
        if (!isLocalFunc(Callee))
          // If IPA, then argument escapes only in a Call of non-local function
          return {EscKindTy::MAY_ESCAPE, EscReasonBits::PASSING_TO_CALL};

        // If called function is local, find argument information in ArgsEscapes
        // provided by IPA callgraph traversal
        const auto FuncIt = ArgsEscapes->get().find(Callee);
        assert(FuncIt != ArgsEscapes->get().end() &&
               "ArgsEscapes must contain information about called function");

        // If it's variadic function, all arguments after fixed ones are escaped
        if (Callee->isVarArg() && (Call->getDataOperandNo(&U) >=
                                   Callee->getFunctionType()->getNumParams()))
          return {EscKindTy::MAY_ESCAPE, EscReasonBits::PASSING_TO_CALL};

        const auto ArgEscIt = FuncIt->second.find(Call->getDataOperandNo(&U));
        assert(ArgEscIt != FuncIt->second.end() &&
               "ArgEscapes must contain information about all arguments");
        if (ArgEscIt->second)
          return {EscKindTy::MAY_ESCAPE, EscReasonBits::PASSING_TO_CALL};
      } else {
        // If not IPA, each call is the escape for each pointer-typed argument
        return {EscKindTy::MAY_ESCAPE, EscReasonBits::PASSING_TO_CALL};
      }
    }
    return {EscKindTy::NO_ESCAPE, std::nullopt};
  }
  case Instruction::Load:
    LLVM_DEBUG(dbgs() << " -- Load\n");
    // Volatile loads make the address observable.
    if (cast<LoadInst>(I)->isVolatile())
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::VOLATILE};
    return {EscKindTy::NO_ESCAPE, std::nullopt};
  case Instruction::Store: {
    LLVM_DEBUG(dbgs() << " -- Store\n");
    // Volatile stores make the address observable.
    if (cast<StoreInst>(I)->isVolatile())
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::VOLATILE};

    if (U.getOperandNo() != 0)
      return {EscKindTy::NO_ESCAPE, std::nullopt};

    // Passing value instead of pointer is neither escape nor alias
    if (const auto *Src = I->getOperand(0); !Src->getType()->isPointerTy())
      return {EscKindTy::NO_ESCAPE, std::nullopt};

    const auto DstObjs = getUnderlyingMayEscObjects(I->getOperand(1));
    if (DstObjs.empty())
      return {EscKindTy::NO_ESCAPE, std::nullopt};

    // Store to GV - escape
    for (const auto *Obj : DstObjs)
      if (isa<GlobalVariable>(Obj))
        return {EscKindTy::MAY_ESCAPE, EscReasonBits::GPTR_ALIASING};

    return {EscKindTy::MAY_ALIASING, DstObjs};
  }
  case Instruction::AtomicRMW: {
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
  case Instruction::AtomicCmpXchg: {
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
  case Instruction::GetElementPtr: {
    LLVM_DEBUG(dbgs() << " -- GetElementPtr\n");
    // AA does not support pointers of vectors, so GEP vector splats need to
    // be considered as captures.
    if (I->getType()->isVectorTy())
      return {EscKindTy::MAY_ESCAPE, EscReasonBits::OTHER};

    // GEP itself is not escape or alias
    return {EscKindTy::NO_ESCAPE, std::nullopt};
  }
  case Instruction::ICmp: {
    LLVM_DEBUG(dbgs() << " -- ICmp\n");
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
        const auto *O =
            I->getOperand(Idx)->stripPointerCastsSameRepresentation();
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

  case Instruction::Ret: {
    LLVM_DEBUG(dbgs() << " -- Ret\n");
    // If not return pointer, means that's not escape
    if (!U->getType()->isPointerTy())
      return {EscKindTy::NO_ESCAPE, std::nullopt};

    // Returning null pointer is not escape
    if (isa<ConstantPointerNull>(U.get()))
      return {EscKindTy::NO_ESCAPE, std::nullopt};

    return {EscKindTy::MAY_ESCAPE, EscReasonBits::RET_PTR};
  }
  default:
    LLVM_DEBUG(dbgs() << " -- Default\n");
    return {EscKindTy::NO_ESCAPE, std::nullopt};
    // Think, maybe we behave too aggressive, because previous logic was
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
const Value *
EscapeAnalysisInfo::getUnderlyingObjectThroughLoads(const Value *&P,
                                                    const unsigned MaxLookup) {
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
/// 1. Pass through LoadInst to get the original loaded object.
/// 2. Ignore phi invariant check.
void EscapeAnalysisInfo::getUnderlyingObjectsWithoutPHIInvCheck(
    const Value *V, SmallVectorImpl<const Value *> &Objects,
    const unsigned MaxLookup) {
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
const Value *EscapeAnalysisInfo::getUnderlyingObjectFromInt(const Value *V) {
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
bool EscapeAnalysisInfo::getUnderlyingObjectsForCodeGenWithoutPHIInvCheck(
    const Value *V, SmallVectorImpl<Value *> &Objects,
    const unsigned MaxLookup) {
  SmallPtrSet<const Value *, 16> Visited;
  SmallVector<const Value *, 4> Working(1, V);
  do {
    V = Working.pop_back_val();

    SmallVector<const Value *, 4> Objs;
    getUnderlyingObjectsWithoutPHIInvCheck(V, Objs, MaxLookup);

    for (const Value *VV : Objs) {
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
                                               const unsigned MaxLookup) {
  SmallVector<Value *, 8> UnderlObjs;
  LLVM_DEBUG(dbgs() << "\tgetUnderlyingMayEscObjects for " << *V << "\n");
  getUnderlyingObjectsForCodeGenWithoutPHIInvCheck(V, UnderlObjs, MaxLookup);
  LLVM_DEBUG(if (!UnderlObjs.empty()) {
    dbgs() << "\tgetUnderlyingMayEscObjects:";
    for (const auto *Obj : UnderlObjs)
      dbgs() << "\t\t" << *Obj << "\n"; }
      else
      dbgs() << "\tgetUnderlyingMayEscObjects -- empty\n"; );
  return UnderlObjs;
}

/// Is Value V is escaping in some path from Entry to BB?
bool EscapeAnalysisInfo::isEscapedForBB(
    const BasicBlock *BB, const Value *V,
    std::optional<std::reference_wrapper<EscReasonTy>> EscReason) const {
  auto CombinedEscReason = isExternalEscapedObject(V);

  if (CombinedEscReason.any() && !EscReason.has_value())
    return true;

  const auto It = BBEscapeStates.find(BB);
  assert((It != BBEscapeStates.end()) &&
         "BBEscapeState must exist for each BB\n");

  const auto EscObjs = It->second.getEscapedObjs().find(V);
  if (EscObjs != It->second.getEscapedObjs().end())
    CombinedEscReason |= EscObjs->second;

  if (EscReason.has_value())
    EscReason->get() = CombinedEscReason;

  return CombinedEscReason.any();
}

/// Is Value V is escaping somewhere in the function
bool EscapeAnalysisInfo::isEscapedForFunc(
    const Value *V,
    std::optional<std::reference_wrapper<EscReasonTy>> EscReason) const {
  EscReasonTy CombinedEscReason;
  for (const auto &BB : AnalyzedFunc) {
    EscReasonTy BBEscReason;

    bool Found = isEscapedForBB(&BB, V, BBEscReason);
    if (Found) {
      if (!EscReason.has_value())
        return true;
      CombinedEscReason |= BBEscReason;
    }
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

  OS << "Escaping objects for BB " << BB->getName() << ":\n";
  for (const auto &V : It->second.getEscapedObjs()) {
    // if (isExternalEscapedObject(V) || isa<Argument>(V))
    if (ArgsEscapes.has_value()) {
      // dbgs() << "\nprintEscReason 1: ";
      // printEscReason(V.second);
      auto IsExtEscReason = isExternalEscapedObject(V.first);
      // dbgs() << "printEscReason 2: ";
      // printEscReason(EscReason);

      if (IsExtEscReason == GPTR_ALIASING)
        continue;

      // printEscReason(PTR_ARG_ALIASING);
      if (V.second == PTR_ARG_ALIASING)
        continue;
    } else {
      if (isExternalEscapedObject(V.first).any())
        // I'm not sure, we should not print objects escaping by definition
        // (such as global variables or pointer arguments),
        // but let's omit them for now
        continue;
    }

    OS << *V.first << "\n";
    // printEscReason(V.second);
  }
  OS << "\n";
}

void EscapeAnalysisInfo::print(raw_ostream &OS) const {
  for (const auto &BB: AnalyzedFunc)
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

//===----------------------------------------------------------------------===//
// Escape analysis global (IPA)
//===----------------------------------------------------------------------===//

void EscapeAnalysisGlobalInfo::setAllPtrArgsEscaped(
    ArgumentEscapesMap &ArgsEscapes, const Function *F) {
  for (const auto &Arg : F->args())
    if (Arg.getType()->isPointerTy())
      ArgsEscapes[F][Arg.getArgNo()] = true;
}

void EscapeAnalysisGlobalInfo::setAllPtrArgsNotEscaped(
    ArgumentEscapesMap &ArgsEscapes, const Function *F) {
  for (const auto &Arg : F->args())
    if (Arg.getType()->isPointerTy())
      ArgsEscapes[F][Arg.getArgNo()] = false;
}

bool EscapeAnalysisGlobalInfo::isRecursiveCallGraphNode(const Function *F,
                                                        CallGraphNode *CGN) {
  for (auto CI = CGN->begin(), CE = CGN->end(); CI != CE; ++CI) {
    CallGraphNode *Callee = CI->second;
    if (Callee && Callee->getFunction() == F)
      return true;
  }
  return false;
}

void EscapeAnalysisGlobalInfo::updFuncArgsEscapes(
    const Function *F, const EscapeAnalysisInfo &EAI) {
  for (const auto &Arg : F->args()) {
    EscapeAnalysisInfo::EscReasonTy ArgEscReason;
    LLVM_DEBUG(dbgs() << "@@@@@@@@@ ESC ARG " << Arg << " -- "
                      << EAI.isEscapedForFunc(&Arg) << "\n";);

    // Argument doesn't escape
    if (!EAI.isEscapedForFunc(&Arg, std::ref(ArgEscReason))) {
      ArgsEscapes[F][Arg.getArgNo()] = false;
      continue;
    }

    // Arguments escapes only by being the pointer argument
    if (ArgEscReason == EscapeAnalysisInfo::PTR_ARG_ALIASING) {
      ArgsEscapes[F][Arg.getArgNo()] = false;
      continue;
    }

    LLVM_DEBUG(EscapeAnalysisInfo::printEscReason(ArgEscReason));
    ArgsEscapes[F][Arg.getArgNo()] = true;
  }
}
EscapeAnalysisGlobalInfo::EscapeAnalysisGlobalInfo(CallGraph &CG) {
  // We do a bottom-up SCC traversal of the call graph.  In other words, we
  // visit all callees before callers (leaf-first).

  // This is needed to (conservatively) consider recursive calls and SCCs.
  // First, find all SCCs and set all pointer argument as escaped
  for (scc_iterator<CallGraph *> It = scc_begin(&CG); !It.isAtEnd(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;
    assert(!SCC.empty() && "SCC with no functions?");

    LLVM_DEBUG(dbgs() << "SCC: " << SCC.size() << "\n";
    for (const CallGraphNode *CGN: SCC) {
      if (CGN->getFunction())
        dbgs() << "\t" << CGN->getFunction()->getName() << "\n";
    });

    if (SCC.size() == 1) {
      // Check if it's recursive call or not
      const auto *F = SCC[0]->getFunction();
      if (!EscapeAnalysisInfo::isLocalFunc(F))
        continue;

      if (isRecursiveCallGraphNode(F, SCC[0])) {
        LLVM_DEBUG(dbgs() << "\t" << F->getName().str() << " is recursive.\n");
        // setAllPtrArgsEscaped(ArgsEscapes, F);
        setAllPtrArgsNotEscaped(ArgsEscapes, F);
      } else {
        LLVM_DEBUG(dbgs() << "\t" << F->getName().str() << " is not recursive.\n");
      }
    } else { // SCC.size() > 1
      for (const CallGraphNode *CGN: SCC) {
        const auto F = CGN->getFunction();
        if (!EscapeAnalysisInfo::isLocalFunc(F))
          continue;
        // setAllPtrArgsEscaped(ArgsEscapes, F);
        setAllPtrArgsNotEscaped(ArgsEscapes, F);
      }
    }
  }

  // Main callgraph traversal
  for (scc_iterator<CallGraph *> It = scc_begin(&CG); !It.isAtEnd(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;
    assert(!SCC.empty() && "SCC with no functions?");

    bool Converged = false;
    while (!Converged) {
      Converged = true;
      for (const CallGraphNode *CGN : SCC) {
        const auto *F = CGN->getFunction();
        if (!EscapeAnalysisInfo::isLocalFunc(F))
          // Just skip, because all externals calls will be treated as escaped
          // during local escape analysis
          continue;

        // const auto [Iter, Inserted] =
        //     FuncEscapeInfo.try_emplace(F, EscapeAnalysisInfo(*F, ArgsEscapes));
        // assert(Inserted && "One function - one insert\n");
        // FuncEscapeInfo[F] = EscapeAnalysisInfo(*F, ArgsEscapes);

        // auto Iter = FuncEscapeInfo.find(F);
        // if (Iter == FuncEscapeInfo.end()) {
          // Iter->second = EscapeAnalysisInfo(*F, ArgsEscapes);
        // } else {
          // const auto [It, Inserted] = FuncEscapeInfo.try_emplace(
              // F, EscapeAnalysisInfo(*F, ArgsEscapes));
          // Iter = It;
          // assert(Inserted && "One function - one insert\n");
        // }

        auto Iter = FuncEscapeInfo.find(F);
        if (Iter != FuncEscapeInfo.end())
          FuncEscapeInfo.erase(Iter);
        const auto [NewIter, Inserted] =
            FuncEscapeInfo.try_emplace(F, EscapeAnalysisInfo(*F, ArgsEscapes));
        assert(Inserted && "One function - one insert\n");
        const auto &EAI = NewIter->second;

        const auto PrevArgsEscapes = ArgsEscapes[F];
        updFuncArgsEscapes(F, EAI);
        if (PrevArgsEscapes != ArgsEscapes[F])
          Converged = false;
        else
          LLVM_DEBUG(dbgs() << "++++++ Func " << F->getName()
                            << " -- Converging ++++++\n";);

        LLVM_DEBUG(dbgs() << "\n";);
        // }
      }
    }
  }
}

void EscapeAnalysisGlobalInfo::print(Module &M, raw_ostream &O) const {
  for (const Function &F: M) {
    if (!EscapeAnalysisInfo::isLocalFunc(&F))
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
  dbgs() << "Running EscapeAnalysisGlobal::run()\n";
  return EscapeAnalysisGlobalInfo(AM.getResult<CallGraphAnalysis>(M));
}

PreservedAnalyses
EscapeAnalysisGlobalPrinterPass::run(Module &M,
                                     ModuleAnalysisManager &AM) const {
  OS << "Printing analysis 'Escape Analysis' for module '" << M.getName()
     << "':\n";
  AM.getResult<EscapeAnalysisGlobal>(M).print(M, OS);
  return PreservedAnalyses::all();
}
