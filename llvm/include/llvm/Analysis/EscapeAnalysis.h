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
#include <functional>

namespace llvm {
/// This is the implementation of simple escape analysis

struct UnderlObjInfo {
  const Value *Obj;
  bool Loaded;
};

/// Interface to access escape analysis results for single function.
class EscapeAnalysisInfo {
  friend class EscapeAnalysisGlobalInfo;

public:
  /// Reasons of escaping for objects
  enum EscReasonBits {
    NO_ESCAPE        = 0,
    GPTR_ALIASING    = 1,
    PTR_ARG_ALIASING = 1 << 1,
    PASSING_TO_CALL  = 1 << 2,
    RET_PTR          = 1 << 3,
    VOLATILE         = 1 << 4,
    ESCAPED_CALL     = 1 << 5,
    OTHER            = 1 << 6,
    INVALID          = 1 << 7
  };
  using EscReasonTy = std::bitset<6>;

  struct IPABottomTopInfoEntry {
    SmallDenseMap<unsigned, EscReasonTy> ArgEscapes; // for each argument
    bool IsRetEscape = false; // whether return value is escaping or not
    bool IsRecursive = false; // whether function is recursive or not
    bool IsPassedAsPtr = false; // whether function is passed as pointer
                                // to another call or not
    bool operator==(const IPABottomTopInfoEntry &Other) const {
      return ArgEscapes == Other.ArgEscapes && IsRetEscape == Other.IsRetEscape;
    }
    bool operator!=(const IPABottomTopInfoEntry &Other) const {
      return !(*this == Other);
    }
  };

  bool getIsRetEscape() const { return IsRetEscape; }

  using IPABottomTopMap = DenseMap<const Function *, IPABottomTopInfoEntry>;
  using IPAArgEscFromCallsMap = DenseMap<const Function *, SmallVector<bool>>;

  static void printEscReason(EscReasonTy EscReason);

  /// Run analysis for given function.
  /// ArgumentEscape is needed for IPA analysis (because we should ignore
  /// escaping by calls)
  explicit EscapeAnalysisInfo(
    const Function &Fn,
    std::shared_ptr<IPABottomTopMap> IPAFuncEscInfo = nullptr,
    std::shared_ptr<IPAArgEscFromCallsMap> IPAArgEscFromCallers_ = nullptr);

  void print(raw_ostream &OS) const;

  /// Recursively search in the instruction for the underlying objects which
  /// may escape
  static SmallVector<UnderlObjInfo> getUnderlyingMayEscObjs(
      const Value *V, unsigned MaxLookup = MaxUnderlObjLookup,
      std::shared_ptr<IPABottomTopMap> IPAFuncEscInfo = nullptr);

  /// Is Value V is escaping somewhere in the function
  bool isEscapedForFunc(const Value *V,
                        std::optional<std::reference_wrapper<EscReasonTy>>
                            EscReason = std::nullopt) const;

  EscReasonTy findObjInBBEscapeState(const BasicBlock *BB,
                                     const Value *V) const;

  /// Return escape reason for V in BB
  EscReasonTy getFullEscapedForBBReason(const BasicBlock *BB, const Value *V) const;

  /// Is Value V is escaping in some path from Entry to BB?
  bool isEscapedForBB(const BasicBlock *BB, const Value *V,
                      EscReasonTy *EscReason = nullptr) const;
  bool isEscapedForBBIPA(const BasicBlock *BB, const Value *V,
                      EscReasonTy *EscReason = nullptr) const;

  void forEachPointeeDo(const Value *Obj, const BasicBlock *BB,
                        std::function<void(const Value *)> Action) const {
    const auto It = BBEscapeStates.find(BB);
    assert(It != BBEscapeStates.end());
    It->second.forEachPointeeDo(Obj, Action);
  }

private:
  // Types of object escaping states
  enum class EscKindTy { NO_ESCAPE, MAY_ESCAPE, MAY_ALIASING };
  static constexpr unsigned MaxUnderlObjLookup = 20;

  // Reference to the function being analyzed.
  const Function &AnalyzedFunc;

  // Resulting type: list of escaping objects
  using EscapedObjectsTy = DenseMap<const Value *, EscReasonTy>;

  // IPA information about arguments escapes (bottom-top)
  std::shared_ptr<IPABottomTopMap> IPABottomTopInfo;

  // IPA information about arguments escapes from calls (top-bottom)
  std::shared_ptr<IPAArgEscFromCallsMap> IPATopDownArgEsc;

  // Whether return value is escaping or not (need it in IPA)
  bool IsRetEscape = false;
  struct EscapeState;

  /// Map of basic blocks to their escape analysis states.
  DenseMap<const BasicBlock *, EscapeState> BBEscapeStates;

  class PointsToRelTy {
    friend struct EscapeState;

    using PointeeListTy = SmallPtrSet<const Value *, 8>;
    DenseMap<const Value *, PointeeListTy> PointsToMap;

  public:
    /// Traverse the (implicit) tree of aliases and get the list of aliases
    std::optional<PointeeListTy> getPointees(const Value *V) const;

    /// We need it to check if something changed in the data-flow analysis
    bool operator==(const PointsToRelTy &Other) const;

    /// Print alias relation
    void print(raw_ostream &OS) const;
  };

  struct EscapeState {
    /// Compare EscapeStates (need in data flow analysis)
    bool operator==(const EscapeState &ES) const;
    bool operator!=(const EscapeState &ES) const { return !(*this == ES); }

    /// Make list of escaping object + its aliases, and add them to the list
    /// of escaping object
    void addEscapingObject(const Value *EscObj, EscReasonTy EscReason);

    /// Adds an object to the list of escaped objects with a specified escape
    /// reason. If the object is already in the list, update escape reason.
    void addEscapeObjOrReason(const Value *EscObj, const EscReasonTy EscReason);

    /// Check CheckedObj escape status (as [maybe] external object)
    /// and update AffectedObj if needed
    void checkAndUpdEscStatus(const Value *CheckedObj, const Value *AffectedObj,
                              const EscapeAnalysisInfo *EAI);

    void forEachPointeeDo(const Value *Obj,
                          std::function<void(const Value *)> Action) const;

    /// Adds an alias relationship between a given alias and a pointee value
    /// in the escape analysis information. If the pointee value has previously
    /// escaped or if the alias itself is an escaping pointer, the alias is
    /// also marked as escaping.
    void addPointsTo(const UnderlObjInfo &Pointer, const UnderlObjInfo &Pointee,
                     const EscapeAnalysisInfo *EAI);

    void merge(const EscapeState &OtherES, const EscapeAnalysisInfo *EAI) {
      mergeAliases(OtherES, EAI);
      mergeEscapedObjects(OtherES);
    }

    const EscapedObjectsTy &getEscapedObjs() const { return EscapedObjs; };
    const PointsToRelTy &getPointsTo() const { return PointsTo; }

    void print(raw_ostream &OS) const;

    /// Try to find object in the EscapedObjects and return escape reason
    EscReasonTy getEscReason(const Value *V) const;

  private:
    // Set of allocations that escape in this block.
    EscapedObjectsTy EscapedObjs;

    // map from Alloca aliases to the original Allocas
    // Note that a Value may be the alias of multiple Allocas
    PointsToRelTy PointsTo;

    /// Merge two Alias relations into one
    void mergeAliases(const EscapeState &OtherES,
                      const EscapeAnalysisInfo *EAI);

    /// Merge lists of escaped objects for two escape states (BBs)
    void mergeEscapedObjects(const EscapeState &OtherES);
  };

  /// Check if function returns escaped object, and update function return
  /// escape status
  void updRetEscStatus(EscapeState &ES, const BasicBlock *BB,
                       const SmallVectorImpl<UnderlObjInfo> &UnderlObjs);
  void addEscapedPtrArgs(EscapeState &ES);

  /// Compute the resulting escape state for BB
  void compBBEscapeState(const BasicBlock *BB, EscapeState &ES);

  /// Merges the escape analysis states from multiple incoming blocks.
  EscapeState mergePredEscapeStates(const BasicBlock *BB);

  /// Check if it's non-constant global variable (which can be modified)
  static bool isNonConstGV(const Value *V);

  /// Check if that's the object is "already escaped":
  /// e.g. pointer function argument or global variable.
  EscReasonTy getExtObjStatusWithArgLookup(const Value *V) const;
  static EscReasonTy getExtObjStatus(const Value *V);

  /// Get escape status of the object and if it's a pointer argument,
  /// lookup in the top-bottom argument escape analysis
  EscReasonTy
  getExtObjStatusWithIPA(const Value *V) const;

  /// Determine what kind of escape behaviour V may exhibit.
  struct EscInfoTy {
    EscKindTy EscKind;
    std::optional<std::variant<EscReasonTy, SmallVector<UnderlObjInfo>>>
        EscDetails = std::nullopt;
  };

  /// Determine what kind of escape behaviour V may exhibit, return
  /// escape reason and list of aliases if applicable.
  EscInfoTy getEscInfoForOpnd(const Use &U) const;

  /// Functions to process operand/instruction pair to get escape status
  EscInfoTy getEscInfoCall(const Use &U, const Instruction *I) const;
  static EscInfoTy getEscInfoLoad(const Instruction *I);
  static EscInfoTy getEscInfoStore(const Use &U, const Instruction *I);
  static EscInfoTy getEscInfoAtomicRMW(const Use &U, const Instruction *I);
  static EscInfoTy getEscInfoAtomicCmpXchg(const Use &U, const Instruction *I);
  static EscInfoTy getEscInfoGetElementPtr(const Instruction *I);
  static EscInfoTy getEscInfoICmp(const Use &U, const Instruction *I);
  static EscInfoTy getEscInfoRet(const Use &U);

  /// Print escaped objects in some path from Entry to BB
  void printEscapingForBB(const BasicBlock *BB, raw_ostream &OS) const;

  /// Taken from CaptureTracker
  static bool isDereferenceableOrNull(const Value *O, const DataLayout &DL);

  /// Check whether type contains pointers
  static bool structContainsPointerType(const Type *Ty);

  /// Escaping state for the function is union of all BBs' escape states
  const EscapedObjectsTy &getFuncEscState() const;

  /// Find in ArgsEscapes given argument and return escape status
  EscReasonTy getArgEscStatus(unsigned ArgNo, const Function *Func) const;

  /// Find argument in the from-callers (top-bottom) escape info
  EscReasonTy getArgEscTopDownIPA(const unsigned ArgNo,
                                         const Function *Func) const;
};

/// Interface to access safety global (interprocedural) analysis results.
class EscapeAnalysisGlobalInfo {
  Module &M;
  DenseMap<const Function *, EscapeAnalysisInfo> FuncEscapeInfo;

  /// Map for escape information for function arguments from inside of the
  /// function (escape comes from inner objects).
  std::shared_ptr<EscapeAnalysisInfo::IPABottomTopMap> IPABottomTopEscInfo =
      std::make_shared<EscapeAnalysisInfo::IPABottomTopMap>();

  /// Map for escape information about argument from outside of the function
  /// (escape comes from passing parameters to the function calls)
  std::shared_ptr<EscapeAnalysisInfo::IPAArgEscFromCallsMap>
  IPATopDownArgEscInfo =
      std::make_shared<EscapeAnalysisInfo::IPAArgEscFromCallsMap>();

  /// Traverse SCCs in the call graph, find recursive functions and SCCs
  /// init IPAFuncEscInfo
  bool traverseSCCsAndInitIPAEscInfo(
      CallGraph &CG, SmallPtrSet<const Function *, 8> &RecursiveFuncs);

  /// Bottom-top traversal of SCCs of the callgraph, do local analysis,
  /// fill FuncEscapeInfo, IPAFuncEscInfo
  bool traverseCGBottomTop(CallGraph &CG,
                                 const SmallPtrSetImpl<const Function *> &
                                 RecursiveFuncs,
                                 SmallVector<std::vector<CallGraphNode *> > &
                                 SCCList);

  /// Init escape status of arguments and return
  static void
  setAllPtrArgsNotEscaped(EscapeAnalysisInfo::IPABottomTopMap &IPAFuncEscInfo,
                          const Function *F);

  /// Return map: Function -> list of call instructions
  DenseMap<const Function *, SmallVector<const CallBase *>>
  getFuncToCallSitesMap();

  /// Check weather function passed to the Objective C selector
  bool isFuncPassedToObjCSelector(const Function *F);

  /// Compute escape status for the function argument based on call instructions
  void traverseCGTopDown(
      const SmallVectorImpl<std::vector<CallGraphNode *>> &SCCList,
      const DenseMap<const Function *, SmallVector<const CallBase *>>
          &FuncCallSites,
      const SmallPtrSetImpl<const Function *> &RecursiveFuncs);

  void printArgEscStatus();

  /// Compute escape status for the function argument based on call instructions
  void evalTopDownArgEscStatus(
      const DenseMap<const Function *, SmallVector<const CallBase *>>
          &FuncCallSites,
      const Function *F) const;

  /// Check if call graph node is the recursive call
  /// (relevant for SCC with 1 node)
  static bool isRecursiveCallGraphNode(const CallGraphNode *CGN);
  void updIPAFuncEscInfo(const Function *F,
                          const EscapeAnalysisInfo &EAI) const;
  void printSCC(const std::vector<CallGraphNode *> &SCC);

public:
  explicit EscapeAnalysisGlobalInfo(CallGraph &CG, Module &M);
  void print(Module &M, raw_ostream &O) const;

  /// For given pointer, get underlying objects, and get escape status for them
  bool
  isEscapedUndrlObjOrPointee(const Value *Addr, const BasicBlock *BB,
                             EscapeAnalysisInfo::EscReasonTy &EscReason);

  /// Is Value V is escaping in some path from Entry to BB in the function F
  bool isEscapedForBBTSan(const Function *F, const BasicBlock *BB,
                          const UnderlObjInfo &UnderlObj,
                          EscapeAnalysisInfo::EscReasonTy &EscReason);

  /// This is needed for using with OuterAnalysisManagerProxy
  bool invalidate(Module &, const PreservedAnalyses &,
                  ModuleAnalysisManager::Invalidator &) { return false; }
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
