//==- EscapeAnalysis.h - Generic Escape Analysis Implementation --==//
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

#include "ValueTracking.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
  /// This is the implementation of simple escape analysis

  class EscapeAnalysisInfo {
  public:
    explicit EscapeAnalysisInfo(const Function &Fn);
    void print(raw_ostream &OS);

  private:
    /// Types of object escaping states
    enum class EscapeKind {
      NO_ESCAPE,
      MAY_ESCAPE,
      ALIASING,
      MAY_ESCAPE_AND_ALIASING
    };
    static const unsigned GetUndrlObjMaxLookup = 20;

    // Reference to the function being analyzed.
    const Function &F;
    using EscapedObjectsTy = DenseSet<const Value *>;

    struct EscapeState;

    class AliasRelationTy {
      friend struct EscapeState;

      using AliasListTy = SmallPtrSet<const Value *, 8>;
      DenseMap<const Value*, AliasListTy> AliasMap;

    public:
      /// Merge two relations into one (Other), save results into current (this)
      void merge(const AliasRelationTy &Other);

      /// Get list of aliases for the object a
      std::optional<AliasListTy> getAliases(const Value *V) const;

      /// We need it to check if something changed in the data-flow analysis
      bool operator==(const AliasRelationTy& Other) const;

      /// Print alias relation
      void print(raw_ostream &OS) const;
    };

    struct EscapeState {
      // const BasicBlock *BB = nullptr;

      // Set of allocations that escape in this block.
      EscapedObjectsTy EscapedObjects;

      // map from Alloca aliases to the original Allocas
      // Note that a Value may be the alias of multiple Allocas
      AliasRelationTy AliasRel;

      // EscapeState(const BasicBlock *BB_) : BB(BB_) {}

      /// Compare EscapeStates (need in data flow analysis)
      bool operator==(const EscapeState &ES) const;
      bool operator!=(const EscapeState &ES) const { return !(*this == ES); }

      /// Make list of escaping object + its aliases, and add them to the list
      /// of escaping object
      void addEscapingObject(const Value *EscapingObject);

      void addAlias(const Value *Alias, const Value *PointeeValue);

      void merge(const EscapeState &OtherES) {
        mergeAliases(OtherES);
        mergeEscapedObjects(OtherES);
      }

    private:
      void getEscapingObjectsList(const Value *EscapingObject,
                                  SmallPtrSetImpl<const Value *> &EscObjList);

      /// Merge two Alias relations into one
      void mergeAliases(const EscapeState &OtherES) {
        for (const auto &[OtherKey, OtherValueSet] : OtherES.AliasRel.AliasMap)
          for (const auto *OtherPointeeValue: OtherValueSet)
            addAlias(OtherKey, OtherPointeeValue);
      }

      void mergeEscapedObjects(const EscapeState &OtherES) {
        // Here we don't need to look through aliases, because if some object
        // has been added to EscapedObjects, then all it's aliases
        // have been added too
        EscapedObjects.insert(OtherES.EscapedObjects.begin(),
                              OtherES.EscapedObjects.end());
      }
    };

    /// Map of basic blocks to their escape analysis states.
    DenseMap<const BasicBlock *, EscapeState> BBEscapeStates;

    /// Compute the resulting escape state for BB
    static void compOutEscapeState(const BasicBlock *BB, EscapeState &ES);

    /// Merges the escape analysis states from multiple incoming blocks.
    EscapeState mergePredEscapeStates(const BasicBlock *BB);

    /// Check if that's the object is "already escaped":
    /// e.g. pointer function argument or global variable.
    static bool isExternalEscapedObject(const Value *V) {
      const Value *UnderlyingObj = getUnderlyingObject(V);

      if (const auto *CI = dyn_cast<CallInst>(UnderlyingObj))
        return CI->getFunctionType()->getReturnType()->isPointerTy();

      return ((isa<Argument>(UnderlyingObj) &&
               UnderlyingObj->getType()->isPointerTy()) ||
              (isa<GlobalVariable>(UnderlyingObj)));
    }

    /// Determine what kind of escape behaviour V may exhibit.
    static std::pair<EscapeKind, std::optional<const Value*>>
        getEscapeKindForPtrOpnd(const Use &U, const Instruction *I);

    /// Print escaped objects in some path from Entry to BB
    void printEscapingForBB(const BasicBlock *BB, raw_ostream &OS);

    /// Taken from CaptureTracker
    static bool isDereferenceableOrNull(const Value *O, const DataLayout &DL);

    /// Check whether type contains pointers
    static bool containsPointerType(const Type *Ty);

    /// Escaping state for the function is the escape state for Exit BB
    const EscapedObjectsTy &getFuncEscState() const;

  public:
    /// Recuresively search in the instruction for the underlying objects which
    /// may escape
    static const Value *getUnderlyingMayEscapingObject(const Value *V);

    /// Recuresively search in the instruction for the underlying objects which
    /// may escape
    static SmallVector<Value *, 8>
    getUnderlyingMayEscObjectsNew(const Value *V);

    /// Is Value V is escaping somewhere in the function
    bool isEscapedForFunc(const Value *V) const {
      return getFuncEscState().contains(V);
    }

    /// Is Value V is escaping in some path from Entry to BB?
    bool isEscapingForBB(const BasicBlock *BB, const Value *V) const;
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
