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

#include "../../../../clang/include/clang/InstallAPI/MachO.h"

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

  private:
    // Reference to the function being analyzed.
    const Function &F;
    using EscapedAllocasTy = DenseSet<const Value *>;

    class AliasRelationTy {
      using AliasListTy = SmallPtrSet<const Value *, 8>;
      DenseMap<const Value*, AliasListTy> AliasMap;

    public:
      /// Merge two relations into one (Other), save results into current (this)
      void merge(const AliasRelationTy &Other);

      /// Add the order of aliases (a, b)
      void addAlias(const Value *Alias, const Value *PointeeValue);

      /// Get list of aliases for the object a
      std::optional<AliasListTy> getAliases(const Value *V) const;

      /// We need it to check if something changed in the data-flow analysis
      bool operator==(const AliasRelationTy& Other) const;

      /// Print alias relation
      void print(raw_ostream &OS) const;
    };

    struct EscapeState {
      // Set of allocations that escape in this block.
      EscapedAllocasTy EscapedAllocas;

      // map from Alloca aliases to the original Allocas
      // Note that a Value may be the alias of multiple Allocas
      AliasRelationTy AliasRel;

      bool operator==(const EscapeState &ES) const {
        if (this == &ES) return true;
        return ((EscapedAllocas == ES.EscapedAllocas) &&
                (AliasRel == ES.AliasRel));
      }

      bool operator!=(const EscapeState &ES) const { return !(*this == ES); }
    };

    /// Map of basic blocks to their escape analysis states.
    DenseMap<const BasicBlock *, EscapeState> BBEscapeStates;

    /// Find escaping alloca in the instruction and add all aliases to the
    /// resulting set of affected allocas
    static std::optional<SmallPtrSet<const AllocaInst *, 8>>
    getAffectedAllocasNew(const Use &Opnd, const AliasRelationTy &AliasRel);

    static void addAliasesToAffectedAllocas(
        const AliasRelationTy &AliasRel, const AllocaInst *EscapedAlloca,
        SmallPtrSet<const AllocaInst *, 8> &AffectedAllocas);

    /// Compute Out set for BB
    static void compOutEscapeState(const BasicBlock *BB,
                            EscapeState &ES);

    /// Merges the escape analysis states from multiple incoming blocks.
    EscapeState mergePredEscapeStates(const BasicBlock *BB);

    /// Determine what kind of capture behaviour V may exhibit.
    static std::pair<EscapeKind, std::optional<const Value*>>
        getEscapeKindForPtrOpnd(const Use &U, const Instruction *I);

    // void printAliasToAlloca(const BasicBlock *BB);
    void printEscaped(const BasicBlock *BB);

    /// Taken from CaptureTracker
    static bool isDereferenceableOrNull(const Value *O, const DataLayout &DL);

    /// Recuresively search for the underlying local object (alloca)
    /// in the instruction
    static const AllocaInst *getUnderlyingAlloca(const Value *V);

    /// Check whether type contains pointers
    static bool containsPointerType(const Type *Ty);

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

  class EscapeAnalysis : public AnalysisInfoMixin<EscapeAnalysis> {
    friend AnalysisInfoMixin<EscapeAnalysis>;
    static AnalysisKey Key;

  public:
    /// Provide the result type for this analysis pass.
    using Result = EscapeAnalysisInfo;

    /// Run the analysis pass
    static Result run(const Function &F, FunctionAnalysisManager &AM);
  };

  /// Printer pass for the \c EscapeAnalysis results.
  class EscapeAnalysisPrinterPass
      : public PassInfoMixin<EscapeAnalysisPrinterPass> {
    raw_ostream &OS;

  public:
    explicit EscapeAnalysisPrinterPass(raw_ostream &OS) : OS(OS) { }

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) const;

    static bool isRequired() { return true; }
  };

} // end namespace llvm

#endif // LLVM_ANALYSIS_ESCAPEANALYSIS_H
