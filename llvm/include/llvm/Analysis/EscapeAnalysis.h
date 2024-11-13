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

#include "llvm/Analysis/CallGraph.h"
#include "ValueTracking.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
/// This is the implementation of simple escape analysis

/// Interface to access escape analysis results for single function.
class EscapeAnalysisInfo {
public:
  /// Run analysis for given function.
  /// ArgumentEscape is needed for IPA analysis (because we should ignore
  /// escaping by calls)
  /// TODO: do we need ArgumentsEscape (with escape reasons system)?
  explicit EscapeAnalysisInfo(const Function &Fn, bool ArgumentsEscape = true);
  void print(raw_ostream &OS);

private:
  /// Types of object escaping states
  enum class EscapeKind { NO_ESCAPE, MAY_ESCAPE, MAY_ALIASING };
  static const unsigned GetUndrlObjMaxLookup = 20;

  // Reference to the function being analyzed.
  const Function &F;
  using EscapedObjectsTy = DenseSet<const Value *>;

  bool ArgumentsEscape;

  struct EscapeState;

  class AliasRelationTy {
    friend struct EscapeState;

    using AliasListTy = SmallPtrSet<const Value *, 8>;
    DenseMap<const Value *, AliasListTy> AliasMap;

  public:
    /// Merge two relations into one (Other), save results into current (this)
    void merge(const AliasRelationTy &Other);

    /// Get list of aliases for the object a
    std::optional<AliasListTy> getAliases(const Value *V) const;

    /// We need it to check if something changed in the data-flow analysis
    bool operator==(const AliasRelationTy &Other) const;

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

    void addAlias(const Value *Alias, const Value *PointeeValue,
                  const EscapeAnalysisInfo *EAI);

    void merge(const EscapeState &OtherES, const EscapeAnalysisInfo *EAI) {
      mergeAliases(OtherES, EAI);
      mergeEscapedObjects(OtherES);
    }

  private:
    void getEscapingObjectsList(const Value *EscapingObject,
                                SmallPtrSetImpl<const Value *> &EscObjList);

    /// Merge two Alias relations into one
    void mergeAliases(const EscapeState &OtherES,
                      const EscapeAnalysisInfo *EAI) {
      for (const auto &[OtherKey, OtherValueSet] : OtherES.AliasRel.AliasMap)
        for (const auto *OtherPointeeValue : OtherValueSet)
          addAlias(OtherKey, OtherPointeeValue, EAI);
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
  void compOutEscapeState(const BasicBlock *BB, EscapeState &ES);

  /// Merges the escape analysis states from multiple incoming blocks.
  EscapeState mergePredEscapeStates(const BasicBlock *BB);

  /// Check if that's the object is "already escaped":
  /// e.g. pointer function argument or global variable.
  bool isExternalEscapedObject(const Value *V) const {
    if (const auto *CI = dyn_cast<CallInst>(V))
      return CI->getFunctionType()->getReturnType()->isPointerTy();

    if (!ArgumentsEscape)
      return isa<GlobalVariable>(V);

    return ((isa<Argument>(V) && V->getType()->isPointerTy()) ||
            (isa<GlobalVariable>(V)));
  }

  /// Determine what kind of escape behaviour V may exhibit.
  static std::pair<EscapeAnalysisInfo::EscapeKind,
                   std::optional<SmallVector<Value *, 8>>>
  getEscapeKindForOpnd(const Use &U);

  /// Print escaped objects in some path from Entry to BB
  void printEscapingForBB(const BasicBlock *BB, raw_ostream &OS);

  /// Taken from CaptureTracker
  static bool isDereferenceableOrNull(const Value *O, const DataLayout &DL);

  /// Check whether type contains pointers
  static bool containsPointerType(const Type *Ty);

  /// Escaping state for the function is the escape state for Exit BB
  const EscapedObjectsTy &getFuncEscState() const;

  /// Custom implementation of getUnderlyingObject infrastracture (taken and
  /// modified from ValueTracker.cpp)
  static const Value *getUnderlyingObjectThroughLoads(const Value *&P,
                                                      unsigned MaxLookup);
  static void getUnderlyingObjectsWithoutPHIInvCheck(
      const Value *V, SmallVectorImpl<const Value *> &Objects,
      unsigned MaxLookup);
  static const Value *getUnderlyingObjectFromInt(const Value *V);
  static bool getUnderlyingObjectsForCodeGenWithoutPHIInvCheck(
      const Value *V, SmallVectorImpl<Value *> &Objects, unsigned MaxLookup);

public:
  /// Recuresively search in the instruction for the underlying objects which
  /// may escape
  static SmallVector<Value *, 8>
  getUnderlyingMayEscObjects(const Value *V,
                             unsigned MaxLookup = GetUndrlObjMaxLookup);

  /// Is Value V is escaping somewhere in the function
  bool isEscapedForFunc(const Value *V) const {
    return getFuncEscState().contains(V);
  }

  /// Is Value V is escaping in some path from Entry to BB?
  bool isEscapedForBB(const BasicBlock *BB, const Value *V) const;
};

/// Interface to access safety global (interprocedural) analysis results.
class EscapeAnalysisGlobalInfo {
  Module *M = nullptr;

public:
  EscapeAnalysisGlobalInfo(Module *M_, CallGraph &CG);
  void print(raw_ostream &O) const;
};

/// EscapeAnalysisInfo wrapper for the new pass manager.
class EscapeAnalysis : public AnalysisInfoMixin<EscapeAnalysis> {
  friend AnalysisInfoMixin<EscapeAnalysis>;
  static AnalysisKey Key;

public:
  using Result = EscapeAnalysisInfo;
  static Result run(const Function &F, FunctionAnalysisManager &AM);
};

/// Printer pass for the \c EscapeAnalysis results.
class EscapeAnalysisPrinterPass
    : public PassInfoMixin<EscapeAnalysisPrinterPass> {
  raw_ostream &OS;

public:
  explicit EscapeAnalysisPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM) const;
  static bool isRequired() { return true; }
};

/// This pass performs the global (interprocedural) escape analysis.
class EscapeAnalysisGlobal : public AnalysisInfoMixin<EscapeAnalysisGlobal> {
  friend AnalysisInfoMixin<EscapeAnalysisGlobal>;
  static AnalysisKey Key;

public:
  using Result = EscapeAnalysisGlobalInfo;
  Result run(Module &M, ModuleAnalysisManager &AM);
};

/// Printer pass for the \c EscapeGlobalAnalysis results.
class EscapeAnalysisGlobalPrinterPass
    : public PassInfoMixin<EscapeAnalysisGlobalPrinterPass> {
  raw_ostream &OS;

public:
  explicit EscapeAnalysisGlobalPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) const;
  static bool isRequired() { return true; }
};

} // end namespace llvm

#endif // LLVM_ANALYSIS_ESCAPEANALYSIS_H
