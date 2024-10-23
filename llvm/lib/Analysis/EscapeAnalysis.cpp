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
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"

#include <deque>

using namespace llvm;

#define DEBUG_TYPE "escape-analysis"
#define DEV_DEBUG_TYPE "escape-analysis-dev"

cl::opt<std::string> PrintEscapeAnalysis(
    "print-escape-analysis", cl::Hidden,
    cl::desc("The option to specify the name of the function "
             "whose escape analysis result is printed."));

//===----------------------------------------------------------------------===//
// Alias relation
//===----------------------------------------------------------------------===//

/// Merge two relations into one (Other), save results into current (this)
void EscapeAnalysisInfo::AliasRelationTy::merge(const AliasRelationTy &Other) {
  for (const auto &[OtherKey, OtherValueSet] : Other.AliasMap)
    AliasMap.insert({OtherKey, OtherValueSet});
}

/// Add the order of aliases (a, b)
void EscapeAnalysisInfo::AliasRelationTy::addAlias(const Value *Alias,
                                                   const Value *PointeeValue) {
  if (Alias == PointeeValue)
    return;
  AliasMap[Alias].insert(PointeeValue);

  // If Alloca A --> Alloca B we assume that B --> A
  if (isa<AllocaInst>(Alias))
    AliasMap[PointeeValue].insert(Alias);
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
// Main analysis
//===----------------------------------------------------------------------===//

EscapeAnalysisInfo::EscapeAnalysisInfo(const Function &Fn): F(Fn) {
  std::deque<const BasicBlock *> WorkList;

  // Try to traverse CFG in reverse post-order
  ReversePostOrderTraversal<const Function *> RPOT(&F);
  for (const BasicBlock *BB : RPOT) {
    WorkList.push_back(BB);
    BBEscapeStates[BB] = EscapeState();
  }

  while (!WorkList.empty()) {
    const BasicBlock *BB = WorkList.front();
    WorkList.pop_front();

    LLVM_DEBUG(errs() << "****************** BB " << BB->getName() << " (func "
                      << Fn.getName() << ") ******************\n");

    EscapeState NewES = mergePredEscapeStates(BB);
    compOutEscapeState(BB, NewES);
    // BBEscapeStates[BB] = NewES;

    // If something changed, proceed with this BB
    LLVM_DEBUG(dbgs() << ">> Check changes for " << BB->getName() << "\n");
    if (NewES != BBEscapeStates[BB]) {
      LLVM_DEBUG(dbgs() << "Changed!\n");
      // Add BB's successors to WorkList and update BB state
      for (const auto *SuccBB : successors(BB)) {
        LLVM_DEBUG(dbgs() << "Add succ " << SuccBB->getName() << "\n");
        WorkList.push_back(SuccBB);
      }
      BBEscapeStates[BB] = NewES;
    }

    LLVM_DEBUG(
      printEscaped(BB);
      BBEscapeStates[BB].AliasRel.print(dbgs());
      errs() << "**** END of BB " <<  BB->getName() << " **** \n\n";
    );
  }
}

void EscapeAnalysisInfo::addAliasesToAffectedAllocas(
    const AliasRelationTy &AliasRel, const AllocaInst *EscapedAlloca,
    SmallPtrSet<const AllocaInst *, 8> &AffectedAllocas) {

  if (const auto Aliases = AliasRel.getAliases(EscapedAlloca);
      (Aliases != std::nullopt)) {
    for (const Value *PointeeAlloca : Aliases.value()) {
      LLVM_DEBUG(dbgs() << "\t\tFOUND ALIAS: " << *EscapedAlloca << " --> "
                        << *PointeeAlloca << "\n");
      assert(isa<AllocaInst>(PointeeAlloca) && "Pointee must be AllocaInst\n");
      AffectedAllocas.insert(cast<AllocaInst>(PointeeAlloca));
    }
  }
}

/// Find escaping alloca in the instruction and add all aliases to the resulting
/// set of affected allocas
std::optional<SmallPtrSet<const AllocaInst *, 8>>
EscapeAnalysisInfo::getAffectedAllocasNew(
    const Use &Opnd, const AliasRelationTy &AliasRel) {
  const AllocaInst *EscapedAlloca = getUnderlyingAlloca(Opnd.get());
  if (!EscapedAlloca)
    return std::nullopt;

  LLVM_DEBUG(dbgs() << "\tFound escaped Alloca: " << *EscapedAlloca << "\n");

  SmallPtrSet<const AllocaInst *, 8> AffectedAllocas;

  AffectedAllocas.insert(EscapedAlloca);
  addAliasesToAffectedAllocas(AliasRel, EscapedAlloca, AffectedAllocas);
  return AffectedAllocas;
}

void EscapeAnalysisInfo::compOutEscapeState(
    const BasicBlock *BB, EscapeState &ES) {
  for (const Instruction &I: *BB) {
    LLVM_DEBUG(dbgs() << "\nI " << I << "\n");
    for (const Use &Opnd: I.operands()) {
      LLVM_DEBUG(dbgs() << "\n\tOPND \t" << *Opnd.get() << "\n";);

      auto [CaptureKnd, Alias] = getEscapeKindForPtrOpnd(Opnd, &I);
      switch (CaptureKnd) {
      case EscapeKind::NO_ESCAPE: { // Nothing to do
        LLVM_DEBUG(dbgs() << "\t-- NO_ESCAPE --\n");
        break;
      }
      case EscapeKind::MAY_ESCAPE: { // Update all affected allocas
        LLVM_DEBUG(dbgs() << "\t-- MAY_ESCAPE --\n");

        auto AffectedAllocas = getAffectedAllocasNew(Opnd, ES.AliasRel);
        if (AffectedAllocas == std::nullopt) break;

        ES.EscapedAllocas.insert(AffectedAllocas.value().begin(),
                                 AffectedAllocas.value().end());
        break;
      }
      case EscapeKind::ALIASING: {
        LLVM_DEBUG(dbgs() << "\t-- ALIASING --\n");
        assert(Alias != std::nullopt && "If found alias, alias must be set\n");
        auto AffectedAllocas =
            getAffectedAllocasNew(Opnd, ES.AliasRel);
        if (AffectedAllocas == std::nullopt) break;

        for (const auto *Alloca : AffectedAllocas.value())
          // If Alias is GEP, find base pointer and add it as alias too
          if (auto *GEP = dyn_cast<GetElementPtrInst>(Alias.value())) {
            // Underlying alloca used in this GEP
            // GEP itself is not an alias
            if (const AllocaInst *GEPUnderlyingAlloca = getUnderlyingAlloca(GEP))
              ES.AliasRel.addAlias(GEPUnderlyingAlloca, Alloca);
          } else {
            // If alias is not GEP, add Alias itself
            ES.AliasRel.addAlias(Alias.value(), Alloca);
          }
        break;
      }
      }
    }
  }
}

EscapeAnalysisInfo::EscapeState EscapeAnalysisInfo::mergePredEscapeStates(
    const BasicBlock *BB) {
  // EscapeState &MergedES = BBEscapeStates[BB];
  EscapeState MergedES;

  // Merge states of predecessors
  for (auto *PredBB : predecessors(BB)) {
    // errs() << "PRED " << PredBB->getName() << "\n";
    EscapeState &PredES = BBEscapeStates[PredBB];

    MergedES.EscapedAllocas.insert(PredES.EscapedAllocas.begin(),
    PredES.EscapedAllocas.end());

    MergedES.AliasRel.merge(PredES.AliasRel);
  }

  // DEBUG
  /*
  errs() << "RESULT OF MERGE\n";
  for (const auto &Pair : MergedES.AliasesToAlloca)
    errs() << "\tALIAS: " << *Pair.first << " --> " << *Pair.second << "\n";
  errs() << "\n";
  for (const auto *V : MergedES.EscapedAllocas)
    errs() << "\tESCAPE " << *V << "\n";
  errs() << "END\n";
  */
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

/// U is the use of the local pointer
/// TODO
///  Aliasing with GEP may occure not only for load, but for ret and for store
///  also and maybe some others ...
// define dso_local ptr @gep_func() {
// entry:
//     %p = alloca ptr, align 8
//     %pGEP = getelementptr inbounds i8, ptr %p, i64 24
//     %pAlias = load i64, ptr %pGEP, align 8
//     %call = call ptr @func()
//     %data = getelementptr inbounds i8, ptr %call, i64 48
//     store i64 %pAlias, ptr %data, align 8
//     ret ptr %pGEP
// }

/// TODO 2
/// Alloca is not only value which can escape. Call e.g. may create
/// a value without Alloca
/// define dso_local ptr @gep_func() {
// entry : % p = alloca ptr, align 8 % pGEP = getelementptr inbounds i8, ptr % p,
//           i64 24 % LoadedGEP = load i64, ptr % pGEP,
//           align 8 % call = call ptr @func() % data = getelementptr inbounds i8,
//           ptr % call, i64 48 store i64 % LoadedGEP, ptr % data,
//           align 8 ret ptr % call
// }

/// TODO 3
/// After mem2reg, there will be no alloca corresponding to the function
/// arguments. Must consider it.
///
std::pair<EscapeKind, std::optional<const Value *>>
EscapeAnalysisInfo::getEscapeKindForPtrOpnd(const Use &U,
                                            const Instruction *I) {
  LLVM_DEBUG(dbgs() << "\tgetEscapeKindForPtrOpnd:\n");

  switch (I->getOpcode()) {
  case Instruction::Call:
  case Instruction::Invoke: {
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
          return {EscapeKind::ALIASING, Call->getArgOperand(0)};
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
        !Call->doesNotCapture(Call->getDataOperandNo(&U))) {
      // The parameter is not marked 'nocapture' - captured.
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    }
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::Load:
    // Volatile loads make the address observable.
    if (cast<LoadInst>(I)->isVolatile())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  case Instruction::VAArg:
    // "va-arg" from a pointer does not cause it to be captured.
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  case Instruction::Store: {
    auto *CE = dyn_cast<ConstantExpr>(I->getOperand(1));
    // Volatile stores make the address observable.
    // Store to global variable is an escape as well
    if ((cast<StoreInst>(I)->isVolatile()) ||
        (isa<GlobalVariable>(I->getOperand(1))) ||
        (CE && isa<GlobalVariable>(CE->getOperand(0))))
      return {EscapeKind::MAY_ESCAPE, std::nullopt};

    if (U.getOperandNo() == 0)
      return {EscapeKind::ALIASING, cast<StoreInst>(I)->getPointerOperand()};

    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::AtomicRMW: {
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
    // AA does not support pointers of vectors, so GEP vector splats need to
    // be considered as captures.
    if (I->getType()->isVectorTy())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};

    // GEP itself is not escape or alias
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  case Instruction::BitCast:
  case Instruction::PHI:
  case Instruction::Select:
  case Instruction::AddrSpaceCast:
    // The original value is not captured via this if the new value isn't.
    return {EscapeKind::ALIASING, I};
  case Instruction::ICmp: {
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
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
    return {EscapeKind::ALIASING, I};

  case Instruction::Ret: {
    // 1. Check if returning the address of alloca directly
    if (isa<AllocaInst>(U.get()->stripPointerCasts()))
      return {EscapeKind::MAY_ESCAPE, std::nullopt};

    // 2. Check if returning a pointer loaded from a stack location
    if (auto *LI = dyn_cast<LoadInst>(U.get())) {
      if (isa<AllocaInst>(LI->getPointerOperand()))
        return {EscapeKind::MAY_ESCAPE, std::nullopt};
    }
    return {EscapeKind::NO_ESCAPE, std::nullopt};
  }
  default:
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

/// Recuresively search for the underlying local object (alloca)
/// in the instruction
const AllocaInst *EscapeAnalysisInfo::getUnderlyingAlloca(const Value *V) {
  // LLVM_DEBUG(dbgs() << "getUnderlyingAlloca() V " << *V << "\n");
  if (auto *Load = dyn_cast<LoadInst>(V))
    // If it's a load, recursively analyze the pointer operand
    return getUnderlyingAlloca(Load->getPointerOperand());

  if (const auto *GEP = dyn_cast<GetElementPtrInst>(V))
    return getUnderlyingAlloca(getUnderlyingObject(GEP));

  // In other cases, return the pointer as is
  if (const auto *Alloca = dyn_cast<const AllocaInst>(V))
    return Alloca;

  // GEP may be computed from global variable, or from a phi-node
  // and both cases seems to be no-escape cases.
  // assert((isa<GlobalVariable>(Ptr) || isa<PHINode>(Ptr) || isa<CallInst>(Ptr)) &&
         // "GEP underlying object is neither AllocaInst nor GlobalVariable");

  return nullptr;
}

void EscapeAnalysisInfo::printEscaped(const BasicBlock *BB) {
  dbgs() << "Escaped allocas for BB " << BB->getName() << ":\n";
  if (BBEscapeStates.find(BB) == BBEscapeStates.end())
    return;
  for (const auto *V : BBEscapeStates[BB].EscapedAllocas)
    dbgs() << *V << "\n";
  dbgs() << "\n";
}

void EscapeAnalysisInfo::print(raw_ostream &OS) {
  const auto FuncEscapingAllocas = BBEscapeStates[&F.back()].EscapedAllocas;
  if (FuncEscapingAllocas.empty())
    return;

  OS << "Escaping variables:\n";
  for (const auto *Alloca : FuncEscapingAllocas)
    OS << *Alloca << "\n";
  OS << "\n";
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
