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

STATISTIC(NumEscapedGPtr, "Number of escaped by assignment to global pointer");
STATISTIC(NumEscapedCall, "Number of escaped by passing to a function");
STATISTIC(NumEscapedRet, "Number of escaped by passing to a function");

//===----------------------------------------------------------------------===//
// Alias relation
//===----------------------------------------------------------------------===//

/// Add the alias: Alias --> PointeeValue
void EscapeAnalysisInfo::EscapeState::addAlias(
    const Value *Alias, const Value *PointeeValue) {
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
  if (const auto ExistingAliases = AliasRel.getAliases(PointeeValue);
      ExistingAliases)
    for (const Value *ExistingAlias : ExistingAliases.value())
      addAlias(Alias, ExistingAlias);

  // FIXME: recheck is it correct
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
void EscapeAnalysisInfo::compOutEscapeState(
    const BasicBlock *BB, EscapeState &ES) {
  for (const Instruction &I: *BB) {
    LLVM_DEBUG(dbgs() << "\nI " << I << "\n");
    for (const Use &Opnd: I.operands()) {
      LLVM_DEBUG(dbgs() << "\n\tOPND \t" << *Opnd.get() << "\n";);

      const auto [EscKind, Alias] = getEscapeKindForPtrOpnd(Opnd, &I);

      switch (EscKind) {
      case EscapeKind::NO_ESCAPE: { // Nothing to do
        LLVM_DEBUG(dbgs() << "\t-- NO_ESCAPE --\n");
        break;
      }
      case EscapeKind::MAY_ESCAPE: { // Update all affected allocas
        LLVM_DEBUG(dbgs() << "\t-- MAY_ESCAPE --\n");

        // if (const Value *EscapingObject =
                // getUnderlyingMayEscapingObject(Opnd.get()); EscapingObject)
          // ES.addEscapingObject(EscapingObject);
        if (const auto EscapingObjects =
                getUnderlyingMayEscapeObjectsNew(Opnd.get());
            !EscapingObjects.empty()) {
          for (const Value *EO: EscapingObjects)
            ES.addEscapingObject(EO);
        }
        break;
      }
      case EscapeKind::ALIASING: {
        LLVM_DEBUG(dbgs() << "\t-- ALIASING --\n");
        assert(Alias != std::nullopt && Alias.value() != nullptr &&
               "If found alias, alias must be set\n");
        LLVM_DEBUG(dbgs() << "\tAlias candidate: " << *Alias.value() << "\n");
        ///////////////////////////////////////////////
        // if (isa<PHINode>(Alias.value())) // DEBUG
          // break;
        // if (isa<SelectInst>(Alias.value())) // DEBUG
          // break;
        ///////////////////////////////////////////////

        // if (const Value *PointeeMayEscapingObject =
              // getUnderlyingMayEscapingObject(Opnd.get());
            // PointeeMayEscapingObject)
          // ES.addAlias(Alias.value(), PointeeMayEscapingObject);

        if (const auto PointeeMayEscapeObjects =
                getUnderlyingMayEscapeObjectsNew(Opnd.get());
            !PointeeMayEscapeObjects.empty()) {
          for (const Value *EO: PointeeMayEscapeObjects)
            ES.addAlias(Alias.value(), EO);
        }

        break;
      }
      case EscapeKind::MAY_ESCAPE_AND_ALIASING: {
        break;
      }
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
std::pair<EscapeAnalysisInfo::EscapeKind, std::optional<const Value *>>
EscapeAnalysisInfo::getEscapeKindForPtrOpnd(const Use &U,
                                            const Instruction *I) {
  LLVM_DEBUG(dbgs() << "\tgetEscapeKindForPtrOpnd -- ");

  switch (I->getOpcode()) {
  case Instruction::Call:
  case Instruction::Invoke: {
    LLVM_DEBUG(dbgs() << "Call/Invoke\n");
    auto *Call = cast<CallBase>(I);

    // Considering llvm.memcpy intrinsic
    if (Call->getCalledFunction() &&
        (Call->getCalledFunction()->getIntrinsicID() == Intrinsic::memcpy) &&
        (Call->getArgOperand(1) == U.get())) {
      // Check whether the source argument is a struct containing pointers
      if (AllocaInst *Alloca = dyn_cast<AllocaInst>(U.get())) {
        auto *StructTy = Alloca->getAllocatedType();
        if (StructTy && containsPointerType(StructTy))
          // First argument (destination) is a new alias
          return {EscapeKind::ALIASING,
                  getUnderlyingMayEscapingObject(Call->getArgOperand(0))};
      }
    }

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
      return {EscapeKind::ALIASING, I};

    // Volatile operations effectively capture the memory location that they
    // load and store to.
    if (auto *MI = dyn_cast<MemIntrinsic>(Call))
      if (MI->isVolatile())
        return {EscapeKind::MAY_ESCAPE, std::nullopt};

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
      // The parameter is not marked 'nocapture' - captured.
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
      NumEscapedCall++;
    }
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::Load:
    LLVM_DEBUG(dbgs() << "Load\n");
    // Volatile loads make the address observable.
    if (cast<LoadInst>(I)->isVolatile())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  case Instruction::VAArg:
    LLVM_DEBUG(dbgs() << "VAArg\n");
    // "va-arg" from a pointer does not cause it to be captured.
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  case Instruction::Store: {
    LLVM_DEBUG(dbgs() << "Store\n");
    // Volatile stores make the address observable.
    if (cast<StoreInst>(I)->isVolatile())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};

    // const auto *Src = I->getOperand(0)->stripPointerCasts();
    // const auto *Dst = I->getOperand(1)->stripPointerCasts();
    const auto *Src =
        getUnderlyingObject(I->getOperand(0)->stripPointerCasts());
    const auto *Dst =
        getUnderlyingObject(I->getOperand(1)->stripPointerCasts());

    // Passing value instead of pointer is neither escape nor alias
    if (!Src->getType()->isPointerTy())
      return {EscapeKind::NO_ESCAPE, std::nullopt};

    // Store to global variable is an escape as well
    // If storing value is not a pointer, that's not escape
    if (auto *CE = dyn_cast<ConstantExpr>(Dst);
        ((isa<GlobalVariable>(Dst)) ||
         (CE && isa<GlobalVariable>(CE->getOperand(0))))) {
      NumEscapedGPtr++;
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    }

    if (U.getOperandNo() == 0)
      return {EscapeKind::ALIASING, getUnderlyingMayEscapingObject(Dst)};
      // Or just
      // return {EscapeKind::ALIASING,
      //   cast<StoreInst>(I)->getPointerOperand()};

    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::AtomicRMW: {
    LLVM_DEBUG(dbgs() << "AtomicRMW\n");
    // atomicrmw conceptually includes both a load and store from
    // the same location.
    // As with a store, the location being accessed is not captured,
    // but the value being stored is.
    // Volatile stores make the address observable.
    auto *ARMWI = cast<AtomicRMWInst>(I);
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
    auto *ACXI = cast<AtomicCmpXchgInst>(I);
    if (U.getOperandNo() == 1 || U.getOperandNo() == 2 || ACXI->isVolatile())
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
  case Instruction::BitCast:      // TODO move
  case Instruction::AddrSpaceCast:// TODO move
    LLVM_DEBUG(dbgs() << "AddrSpaceCast\n");
    // The original value is not captured via this if the new value isn't.
    return {EscapeKind::ALIASING, I};
  case Instruction::ICmp: {
    LLVM_DEBUG(dbgs() << "ICmp\n");
    unsigned Idx = U.getOperandNo();
    unsigned OtherIdx = 1 - Idx;
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

  case Instruction::FCmp: // ICmp we addressed above

  // Binary arithmetical operators
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction::FSub:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::FRem:

  // Logical operators
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:

  // Cast operators
  case Instruction::Trunc:
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::FPTrunc:
  case Instruction::FPExt:

    // Treat binary operators as not escaping
    LLVM_DEBUG(dbgs() << "Binary operator\n");
    return {EscapeKind::NO_ESCAPE, std::nullopt};

  case Instruction::Alloca:
    LLVM_DEBUG(dbgs() << "Alloca\n");
    return {EscapeKind::NO_ESCAPE, std::nullopt};

  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
    LLVM_DEBUG(dbgs() << "PtrToInt/IntToPtr\n");
    return {EscapeKind::ALIASING, I};

  case Instruction::Ret: {
    LLVM_DEBUG(dbgs() << "Ret\n");

    if (!U->getType()->isPointerTy())
      return {EscapeKind::NO_ESCAPE, std::nullopt};

    // 1. Check if returning the address of alloca directly
    const Value *StrippedOpnd = U.get()->stripPointerCasts();

    if (isa<AllocaInst>(StrippedOpnd)) {
      NumEscapedRet++;
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    }

    // 2. Check if returning a pointer loaded from a stack location
    if (auto *LI = dyn_cast<LoadInst>(StrippedOpnd)) {
      if (isa<AllocaInst>(LI->getPointerOperand()) &&
          LI->getPointerOperandType()->isPointerTy())
        return {EscapeKind::MAY_ESCAPE, std::nullopt};
    }

    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::PHI:
  case Instruction::Select:
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  default:
    LLVM_DEBUG(dbgs() << "Default\n");
    // Something else - be conservative and say it is escaped.
    return {EscapeKind::MAY_ESCAPE, std::nullopt};
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

/// Recuresively search in the instruction for the underlying objects which
/// may escape
SmallPtrSet<const Value *, 8>
EscapeAnalysisInfo::getUnderlyingMayEscapeObjectsNew(const Value *V) {
  SmallPtrSet<const Value *, 8> MayEscapeObjects;
  getUnderlyingMayEscapeObjectsNewImpl(V, MayEscapeObjects);
  LLVM_DEBUG(if (!MayEscapeObjects.empty()) {
    dbgs() << "\tMayEscapeObjects:\n";
    for (auto *Obj: MayEscapeObjects)
      dbgs() << "\t\t" << *Obj << "\n";
  });
  return MayEscapeObjects;
}

// TODO rewrite with switch
// TODO when insert, check if it's already contained in the resulting list
void EscapeAnalysisInfo::getUnderlyingMayEscapeObjectsNewImpl(
    const Value *V, SmallPtrSetImpl<const Value *> &MayEscapeObjects) {
  const Value *UndrV = getUnderlyingObject(V);

  LLVM_DEBUG(dbgs() << "\tgetUnderlyingEscapingObject() V " << *UndrV << "\n");
  if (const auto *Load = dyn_cast<LoadInst>(UndrV)) {
    // If it's a load, recursively analyze the pointer operand
    getUnderlyingMayEscapeObjectsNewImpl(Load->getPointerOperand(),
                                         MayEscapeObjects);
    return;
  }

  // This is the object which can escape
  // AllocaInst, Argument - may escape or not escape
  // GlobalVariable - escapes by definition
  if ((isa<AllocaInst>(UndrV)) || (isa<Argument>(UndrV)) ||
      (isa<GlobalVariable>(UndrV))) {
    MayEscapeObjects.insert(UndrV);
    return;
  }

  // Value is an instruction which use many objects which can escape
  if (isa<PHINode>(UndrV) || isa<SelectInst>(UndrV)) {
    for (const auto &U: cast<Instruction>(UndrV)->operands())
      getUnderlyingMayEscapeObjectsNewImpl(U.get(), MayEscapeObjects);
    return;
  }

  if (const auto *CI = dyn_cast<CastInst>(UndrV)) {
    getUnderlyingMayEscapeObjectsNewImpl(CI->getOperand(0), MayEscapeObjects);
    return;
  }

  if (const auto *CI = dyn_cast<CallInst>(UndrV);
      CI && CI->getFunctionType()->getReturnType()->isPointerTy()) {
    MayEscapeObjects.insert(UndrV);
    return;
  }
}

/// Get underlying object which may escape
const Value *
EscapeAnalysisInfo::getUnderlyingMayEscapingObject(const Value *V) {
  const Value *UndrV = getUnderlyingObject(V);

  LLVM_DEBUG(dbgs() << "\tgetUnderlyingEscapingObject() V " << *UndrV << "\n");
  if (const auto *Load = dyn_cast<LoadInst>(UndrV))
    // If it's a load, recursively analyze the pointer operand
    return getUnderlyingMayEscapingObject(Load->getPointerOperand());

  // This is the object which can escape
  // AllocaInst, Argument - may escape or not escape
  // GlobalVariable - escapes by definition
  if ((isa<AllocaInst>(UndrV)) || (isa<Argument>(UndrV)) ||
      (isa<GlobalVariable>(UndrV)))
    return UndrV;

  if (isa<PHINode>(UndrV))
    return UndrV; // FIXME is it only way?

  if (const auto *CI = dyn_cast<CastInst>(UndrV))
    return getUnderlyingObject(CI->getOperand(0));

  if (isa<SelectInst>(UndrV))
    return UndrV; // FIXME is it only way?

  // if (V->getType()->isPointerTy())
    // return V;

  if (const auto *CI = dyn_cast<CallInst>(UndrV);
      CI && CI->getFunctionType()->getReturnType()->isPointerTy())
    return UndrV;

  return nullptr;
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
