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
#include "llvm/Analysis/CallGraph.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include <bitset>
#include <variant>

namespace llvm {
/// This is the implementation of simple escape analysis

using ArgumentEscapesMap =
    DenseMap<const Function *, SmallDenseMap<unsigned, bool>>;

/// Interface to access escape analysis results for single function.
class EscapeAnalysisInfo {
public:
  /// Run analysis for given function.
  /// ArgumentEscape is needed for IPA analysis (because we should ignore
  /// escaping by calls)
  explicit EscapeAnalysisInfo(
      const Function &Fn,
      std::optional<std::reference_wrapper<ArgumentEscapesMap>> ArgsEsc =
          std::nullopt);
  void print(raw_ostream &OS) const;

private:
  // Types of object escaping states
  enum class EscKindTy { NO_ESCAPE, MAY_ESCAPE, MAY_ALIASING };
  static constexpr unsigned GetUnderlObjMaxLookup = 20;

  // Reference to the function being analyzed.
  const Function &AnalyzedFunc;

  // Reasons of escaping
  enum EscReasonBits {
    GPTR_ALIASING = 1,
    PTR_ARG_ALIASING = 1 << 1,
    PASSING_TO_CALL = 1 << 2,
    RET_PTR = 1 << 3,
    VOLATILE = 1 << 4,
    OTHER = 1 << 5,
    INVALID = 1 << 6
  };

  using EscReasonTy = std::bitset<6>;
  void printEscReason(EscReasonTy EscReason) {
    if (EscReason[0]) dbgs() << "GPTR_ALIASING ";
    if (EscReason[1]) dbgs() << "PTR_ARG_ALIASING ";
    if (EscReason[2]) dbgs() << "PASSING_TO_CALL ";
    if (EscReason[3]) dbgs() << "RET_PTR ";
    if (EscReason[4]) dbgs() << "VOLATILE ";
    if (EscReason[5]) dbgs() << "OTHER ";
    dbgs() << "\n";
  }

  // Resulting type: list of escaping objects
  using EscapedObjectsTy = DenseMap<const Value *, EscReasonTy>;

  // IPA information about arguments escapes
  const std::optional<std::reference_wrapper<ArgumentEscapesMap>> ArgsEscapes;

  // Whether return value is escaping or not (need it in IPA)
  bool IsRetEscape = false;

  struct EscapeState;

  /// Map of basic blocks to their escape analysis states.
  DenseMap<const BasicBlock *, EscapeState> BBEscapeStates;

  class AliasRelationTy {
    friend struct EscapeState;

    using AliasListTy = SmallPtrSet<const Value *, 8>;
    DenseMap<const Value *, AliasListTy> AliasMap;

  public:
    /// Traverse the (implicit) tree of aliases and get the list of aliases
    std::optional<AliasListTy> getAliases(const Value *V) const;

    /// We need it to check if something changed in the data-flow analysis
    bool operator==(const AliasRelationTy &Other) const;

    /// Print alias relation
    void print(raw_ostream &OS) const;
  };

  struct EscapeState {
    /// Compare EscapeStates (need in data flow analysis)
    bool operator==(const EscapeState &ES) const;
    bool operator!=(const EscapeState &ES) const { return !(*this == ES); }

    /// Make list of escaping object + its aliases, and add them to the list
    /// of escaping object
    void addEscapingObject(const Value *EscapingObject, EscReasonTy EscReason);

    void addAlias(const Value *Alias, const Value *PointeeValue,
                  const EscapeAnalysisInfo *EAI);

    void merge(const EscapeState &OtherES, const EscapeAnalysisInfo *EAI) {
      mergeAliases(OtherES, EAI);
      mergeEscapedObjects(OtherES);
    }

    const EscapedObjectsTy &getEscapedObjs() const { return EscapedObjects; };
    const AliasRelationTy &getAliases() const { return AliasRel; }

  private:
    // Set of allocations that escape in this block.
    EscapedObjectsTy EscapedObjects;

    // map from Alloca aliases to the original Allocas
    // Note that a Value may be the alias of multiple Allocas
    AliasRelationTy AliasRel;

    void getAliasSubtreeAsList(const Value *Obj,
                               SmallPtrSetImpl<const Value *> &AliasList);

    /// Merge two Alias relations into one
    void mergeAliases(const EscapeState &OtherES,
                      const EscapeAnalysisInfo *EAI);

    /// Merge lists of escaped objects for two escape states (BBs)
    void mergeEscapedObjects(const EscapeState &OtherES);
  };

  /// Compute the resulting escape state for BB
  void compBBEscapeState(const BasicBlock *BB, EscapeState &ES);

  /// Merges the escape analysis states from multiple incoming blocks.
  EscapeState mergePredEscapeStates(const BasicBlock *BB);

  /// Check if that's the object is "already escaped":
  /// e.g. pointer function argument or global variable.
  EscReasonTy isExternalEscapedObject(const Value *V) const;

  /// Determine what kind of escape behaviour V may exhibit.
  struct EscInfoTy {
    EscKindTy EscKind;
    std::optional<std::variant<EscReasonTy, SmallVector<Value *, 8>>>
        EscDetails = std::nullopt;
  };

  /// Determine what kind of escape behaviour V may exhibit, return
  /// escape reason and list of aliases if applicable.
  EscInfoTy getEscapeKindForOpnd(const Use &U) const;

  /// Print escaped objects in some path from Entry to BB
  void printEscapingForBB(const BasicBlock *BB, raw_ostream &OS) const;

  /// Taken from CaptureTracker
  static bool isDereferenceableOrNull(const Value *O, const DataLayout &DL);

  /// Check whether type contains pointers
  static bool structContainsPointerType(const Type *Ty);

  /// Escaping state for the function is the escape state for Exit BB
  /// TODO: remove
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
                             unsigned MaxLookup = GetUnderlObjMaxLookup);

  /// Is Value V is escaping somewhere in the function
  bool isEscapedForFunc(const Value *V) const {
    for (const auto &BB: AnalyzedFunc)
      if (isEscapedForBB(&BB, V))
        return true;
    return false;
    // return getFuncEscState().contains(V);
  }

  /// Is Value V is escaping in some path from Entry to BB?
  bool isEscapedForBB(const BasicBlock *BB, const Value *V) const;

  static bool isLocalFunc(const Function *F) {
    return F && !F->isDeclaration() && F->isDefinitionExact();
  }
};

/// Interface to access safety global (interprocedural) analysis results.
class EscapeAnalysisGlobalInfo {
  DenseMap<const Function *, EscapeAnalysisInfo> FuncEscapeInfo;

  static void setAllPtrArgsEscaped(ArgumentEscapesMap &ArgsEscapes,
                                   const Function *F);

  /// Check if call graph node is the recursive call
  /// (relevant for SCC with 1 node)
  static bool isRecursiveCallGraphNode(const Function *F, CallGraphNode *CGN);

  /// Map to store escape information for function arguments.
  ArgumentEscapesMap ArgsEscapes;

public:
  explicit EscapeAnalysisGlobalInfo(CallGraph &CG);
  void print(Module &M, raw_ostream &O) const;

  /// Is Value V is escaping in some path from Entry to BB in the function F
  bool isEscapedForBBInFunc(const Function *F, const BasicBlock *BB,
                            const Value *V) const {
    if (const auto It = FuncEscapeInfo.find(F); It != FuncEscapeInfo.end())
      return It->second.isEscapedForBB(BB, V);
    return true;
  }

  /// This is needed for using with OuterAnalysisManagerProxy
  bool invalidate(Module &, const PreservedAnalyses &,
                  ModuleAnalysisManager::Invalidator &) {
    return false;
  }
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
