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

EscapeAnalysisInfo::EscapeAnalysisInfo(const Function &Fn): F(Fn) {
  std::deque<const BasicBlock *> WorkList;

  // Try to traverse CFG in reverse post-order
  ReversePostOrderTraversal<const Function *> RPOT(&F);
  for (const BasicBlock *BB : RPOT) {
    WorkList.push_back(BB);
    BBEscapeStates[BB] = EscapeState();
  }

  // errs() << "--------------- FSEscapeAnalysis for " << F.getName() << " --------------- \n";
  while (!WorkList.empty()) {
    const BasicBlock *BB = WorkList.front();
    WorkList.pop_front();

    LLVM_DEBUG(errs() << "****************** BB " << BB->getName()
                      << " ******************\n");

    EscapeState NewES = mergePredEscapeStates(BB);
    compOutEscapeState(BB, NewES);

    // If something changed, proceed with this BB
    if (NewES != BBEscapeStates[BB]) {
      // Add BB's successors to WorkList and update BB state
      for (const auto *SuccBB : successors(BB))
        WorkList.push_back(SuccBB);
      BBEscapeStates[BB] = NewES;
    }

    LLVM_DEBUG(
      printEscaped(BB);
      printAliasToAlloca(BB);
      errs() << "**** END of BB " <<  BB->getName() << " **** \n\n";
    );
  }
}

void EscapeAnalysisInfo::getAffectedAllocas(const EscapeState &InES,
                                            const Value *Opnd,
                                            DenseSet<const AllocaInst *>
                                            &AffectedAllocas) {
  // If that's load instruction, it may be the load of an alias to alloca
  if (const auto *LI = dyn_cast<const LoadInst>(Opnd)) {
    LLVM_DEBUG(dbgs() << "\t\tLoadInst " << *LI << "\n");
    return getAffectedAllocas(InES, LI->getPointerOperand(), AffectedAllocas);
  }

  // 1. Operand may be the alloca object
  if (const auto *Alloca = dyn_cast<const AllocaInst>(
      getUnderlyingObject(Opnd)))
    AffectedAllocas.insert(Alloca);

  // 2. Or it maybe an alias to some alloca
  if (const auto AliasesIt = InES.AliasesToAlloca.find(Opnd);
    AliasesIt != InES.AliasesToAlloca.end())
    for (auto *Alloca : AliasesIt->second) {
      LLVM_DEBUG(
          dbgs() << "\t\tFOUND ALIAS: " << *Opnd << " --> " << *Alloca << "\n");
      if (!AffectedAllocas.contains(Alloca))
        getAffectedAllocas(InES, Alloca, AffectedAllocas);
    }
}

void EscapeAnalysisInfo::addAlias(EscapeState &InOutES,
                                  const AllocaInst *Alloca,
                                  const Value *Alias) {
  LLVM_DEBUG(dbgs() << "\t\tADD ALIAS: " << *Alias << " --> " << *Alloca << "\n");
  InOutES.AliasesToAlloca[Alias].push_back(Alloca);
}

void EscapeAnalysisInfo::compOutEscapeState(
    const BasicBlock *BB, EscapeState &InOutES) {
  for (const Instruction &I: *BB) {
    LLVM_DEBUG(dbgs() << "\nI \t" << I << "\n");
    for (const Use &Opnd: I.operands()) {
      LLVM_DEBUG(dbgs() << "\n\tOPND \t" << *Opnd.get() << "\n";);

      DenseSet<const AllocaInst *> AffectedAllocas;
      getAffectedAllocas(InOutES, Opnd.get(), AffectedAllocas);
      if (AffectedAllocas.empty())
        continue;

      LLVM_DEBUG(for (const auto *Alloca : AffectedAllocas)
                    dbgs() << "\tAFFTD ALLOCA: \t" << *Alloca << "\n";);

      auto [CaptureKnd, Alias] = getEscapeKindForPtrOpnd(Opnd, &I);
      switch (CaptureKnd) {
      case EscapeKind::NO_ESCAPE:
        LLVM_DEBUG(dbgs() << "\t-- NO_ESCAPE --\n");
        // Nothing to do
        break;
      case EscapeKind::MAY_ESCAPE:
        LLVM_DEBUG(dbgs() << "\t-- MAY_ESCAPE --\n");
        // This operand escape. So update all affected allocas
        InOutES.EscapedAllocas.insert(AffectedAllocas.begin(),
                                      AffectedAllocas.end());
        break;
      case EscapeKind::ALIASING:
        LLVM_DEBUG(dbgs() << "\t-- ALIASING --\n");
        // Add a new alias
        assert(Alias != std::nullopt);
        for (const auto *Alloca : AffectedAllocas) {
          addAlias(InOutES, Alloca, Alias.value());

          // If Alias is GEP, find base pointer and add it as alias too
          if (auto *GEP = dyn_cast<GetElementPtrInst>(Alias.value())) {
            // Underlying alloca used in this GEP
            const AllocaInst *BaseAlloca = getBaseAllocaForAliasing(GEP);
            LLVM_DEBUG(dbgs() << "\t\tBaseAlloca:\t" << *BaseAlloca << "\n");
            assert(BaseAlloca && "BaseAlloca in aliasing must be non-null\n");

            addAlias(InOutES, Alloca, BaseAlloca);
          }
        }
        break;
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

    MergedES.AliasesToAlloca.insert(PredES.AliasesToAlloca.begin(),
                                  PredES.AliasesToAlloca.end());
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
bool EscapeAnalysisInfo::containsPointerType(Type *Ty) {
  if (Ty->isPointerTy())
    return true;

  if (!Ty->isStructTy())
    return false;

  for (Type *EltTy : Ty->subtypes())
    if (containsPointerType(EltTy))
      return true;
  return false;
}

/// U is the use of the local pointer
std::pair<EscapeKind, std::optional<const Value*>>
  EscapeAnalysisInfo::getEscapeKindForPtrOpnd(const Use &U, const Instruction *I) {
  LLVM_DEBUG(
    dbgs() << "\tgetCaptureKindForPtrOpnd:\n";
    dbgs() << "\t\tI \t" << *I << "\n";
    dbgs() << "\t\tOPND \t" << *U.get() << "\n";
  );

  switch (I->getOpcode()) {
  case Instruction::Call:
  case Instruction::Invoke: {
    auto *Call = cast<CallBase>(I);

    // Considering llvm.memcpy intrinsic
    if (Call->getCalledFunction() &&
        (Call->getCalledFunction()->getIntrinsicID() == Intrinsic::memcpy) &&
        (Call->getArgOperand(1) == U.get())) {
      // Check whether the source argument is a struct containing pointers
      AllocaInst *Alloca = dyn_cast<AllocaInst>(U.get());

      if (Alloca) {
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
    // This is the main different of the new algorithm.
    // Now we don't consider each store of the pointer to memrory as an escape.
    //
    // Volatile stores make the address observable.
    auto *CE = dyn_cast<ConstantExpr>(I->getOperand(1));
    if ((cast<StoreInst>(I)->isVolatile()) ||
        (isa<GlobalVariable>(I->getOperand(1))) ||
        (CE && isa<GlobalVariable>(CE->getOperand(0))))
      return {EscapeKind::MAY_ESCAPE, std::nullopt};

    if (U.getOperandNo() == 0)
      return {EscapeKind::ALIASING,
              std::optional<const Value *>(
                  std::in_place, cast<StoreInst>(I)->getPointerOperand())};
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
  case Instruction::GetElementPtr:
    // AA does not support pointers of vectors, so GEP vector splats need to
    // be considered as captures.
    if (I->getType()->isVectorTy())
      return {EscapeKind::MAY_ESCAPE, std::nullopt};
    return {EscapeKind::ALIASING, I};
    // return {CaptureKind::ALIASING,
            // cast<GetElementPtrInst>(I)->getPointerOperand()};
  case Instruction::BitCast:
  case Instruction::PHI:
  case Instruction::Select:
  case Instruction::AddrSpaceCast:
    // The original value is not captured via this if the new value isn't.
    return {EscapeKind::ALIASING, std::nullopt};
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
    dbgs() << "ELSE\n";
    // Something else - be conservative and say it is escaped.
    return {EscapeKind::MAY_ESCAPE, std::nullopt};
  }
}

bool EscapeAnalysisInfo::isDereferenceableOrNull(Value *O, const DataLayout &DL) {
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

/// Recursively searches for the base pointer that might be associated with an AllocaInst
const AllocaInst *EscapeAnalysisInfo::getBaseAllocaForAliasing(
    const Value *Ptr) {
  if (auto *Load = dyn_cast<LoadInst>(Ptr))
    // If it's a load, recursively analyze the pointer operand
    return getBaseAllocaForAliasing(Load->getPointerOperand());

  if (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr))
    // If it's a GEP, recursively analyze its base pointer
    return getBaseAllocaForAliasing(GEP->getPointerOperand());

  // In other cases, return the pointer as is
  assert(isa<const AllocaInst>(Ptr));
  return cast<const AllocaInst>(Ptr);
}

void EscapeAnalysisInfo::printAliasToAlloca(const BasicBlock *BB) {
  dbgs() << "AliasToAlloca for BB " << BB->getName() << ":\n";
  if (BBEscapeStates.find(BB) == BBEscapeStates.end())
    return;
  for (const auto &Pair : BBEscapeStates[BB].AliasesToAlloca) {
    dbgs() << "\tAlias: " << *Pair.first << "\n";
    for (const AllocaInst *Alloca: Pair.second)
      dbgs() << "\t\t --> Alloca: " << *Alloca << "\n";
  }
  dbgs() << "\n";
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

EscapeAnalysis::Result EscapeAnalysis::run(Function &F,
                                           FunctionAnalysisManager &AM) {
  EscapeAnalysisInfo EAI(F);
  return EAI;
}

PreservedAnalyses
EscapeAnalysisPrinterPass::run(Function &F, FunctionAnalysisManager &AM) {
  OS << "Printing analysis 'Escape Analysis' for function '"
      << F.getName() << "':\n";
  AM.getResult<EscapeAnalysis>(F).print(OS);
  return PreservedAnalyses::all();
}
