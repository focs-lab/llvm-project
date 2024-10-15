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

#ifndef LLVM_ANALYSIS_ESCAPEANALYSIS_H
#define LLVM_ANALYSIS_ESCAPEANALYSIS_H

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

/// Types of use capture kinds, see \p DetermineUseCaptureKind.
enum class EscapeKind {
  NO_ESCAPE,
  MAY_ESCAPE,
  ALIASING,
};

namespace llvm {
  /// This is the implementation of simple escape analysis

  class EscapeAnalysisInfo {
  public:
    explicit EscapeAnalysisInfo(const Function &Fn);

    void print(raw_ostream &OS);
    using EscapedAllocasTy = DenseSet<const Value *>;

  private:
    // Reference to the function being analyzed.
    const Function &F;

    struct EscapeState {
      // Set of allocations that escape in this block.
      EscapedAllocasTy EscapedAllocas;

      // map from Alloca alias (e.g. GEP) to the original Allocas
      // Note that a Value may be the alias of multiple Allocas
      DenseMap<const Value *, SmallVector<const AllocaInst *>> AliasesToAlloca;

      bool operator!=(const EscapeState &Other) const {
        return ((EscapedAllocas != Other.EscapedAllocas) ||
                (AliasesToAlloca != Other.AliasesToAlloca));
      }
    };

    /// Map of basic blocks to their escape analysis states.
    DenseMap<const BasicBlock *, EscapeState> BBEscapeStates;

    void getAffectedAllocas(const EscapeState &InES,
                             const Value *Opnd,
                             DenseSet<const AllocaInst *> &AffectedAllocas);

    /// Add alias to the set of aliases
    void addAlias(EscapeState &InOutES, const AllocaInst *Alloca, const Value *Alias);

    /// Compute Out set for BB
    void compOutEscapeState(const BasicBlock *BB,
                            EscapeState &InOutES);

    /// Merges the escape analysis states from multiple incoming blocks.
    EscapeState mergePredEscapeStates(const BasicBlock *BB);

    /// Determine what kind of capture behaviour V may exhibit.
    std::pair<EscapeKind, std::optional<const Value*>>
        getEscapeKindForPtrOpnd(const Use &U, const Instruction *I);

    void printAliasToAlloca(const BasicBlock *BB);
    void printEscaped(const BasicBlock *BB);

    /// Taken from CaptureTracker
    bool isDereferenceableOrNull(Value *O, const DataLayout &DL);

    /// Recursively searches for the base pointer that might be associated with an AllocaInst
    /// TODO getUnderlyingObj??
    const AllocaInst *getUnderlyingAllocaForAliasing(const Value *Ptr);

    /// Check whether type contains pointers
    bool containsPointerType(Type *Ty);

  public:

    const EscapedAllocasTy &getFuncEscState() const {
      auto  It  =  BBEscapeStates.find(&F.back());
      assert(It != BBEscapeStates.end() &&
             "Escape state for exit  block  not  found");
      return  It->second.EscapedAllocas;
    }

    bool isEscapedInFunc(const Value *V) const {
      return getFuncEscState().contains(V);
    }
  };

  class EscapeAnalysis
      : public AnalysisInfoMixin<EscapeAnalysis> {
    friend AnalysisInfoMixin<EscapeAnalysis>;

    static AnalysisKey Key;

  public:
    /// Provide the result type for this analysis pass.
    using Result = EscapeAnalysisInfo;

    /// Run the analysis pass
    Result run(Function &F, FunctionAnalysisManager &AM);
  };

  /// Printer pass for the \c EscapeAnalysis results.
  class EscapeAnalysisPrinterPass
      : public PassInfoMixin<EscapeAnalysisPrinterPass> {
    raw_ostream &OS;

  public:
    explicit EscapeAnalysisPrinterPass(raw_ostream &OS) : OS(OS) { }

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

    static bool isRequired() { return true; }
  };

} // end namespace llvm

#endif // LLVM_ANALYSIS_ESCAPEANALYSIS_H
