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

#include "llvm/ADT/SmallSet.h"
#include "llvm/Analysis/CallGraph.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include <bitset>
#include <fstream>
#include <functional>
#include <variant>

namespace llvm {
/// This is the implementation of simple escape analysis

using FieldPathTy = SmallVector<unsigned>;

/// The empty field path, used as the default and as a map key.
///
/// Must be inline: as a plain namespace-scope definition in a header, every
/// translation unit including this file emitted its own, and linking any two
/// of them together failed with a duplicate symbol. Incremental builds never
/// showed it because the archive members that collide were not all being
/// re-linked; a build from a clean tree did.
inline const FieldPathTy EmptyFieldPath;

// Because that's field-sensitive analysis, we distinguish accesses to different
// field of structures. That's why it's not enough to store a pointer to the
// object (e.g. escaped) but to the field within it (if the object is a
// structure)
struct ObjAndPath {
  // Object (can be variable, pointer, structure or array)
  const Value *Obj = nullptr;

  // GEP path to the field (if it's the structure field)
  FieldPathTy Path = EmptyFieldPath;

  bool operator<(const ObjAndPath &Other) const {
    if (Obj != Other.Obj)
      return Obj < Other.Obj;

    return Path < Other.Path;
  }

  bool operator==(const ObjAndPath &Other) const {
    return Obj == Other.Obj && Path == Other.Path;
  }
};

struct UnderlObjTy : ObjAndPath {
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
    bool IsRetEscape = false;   // whether return value is escaping or not
    bool IsRecursive = false;   // whether function is recursive or not
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
  using NonEscapingFuncsMap = std::map<std::string, SmallSet<unsigned, 4>>;

  static void printEscReason(EscReasonTy EscReason);

  /// Run analysis for given function.
  /// ArgumentEscape is needed for IPA analysis (because we should ignore
  /// escaping by calls)
  explicit EscapeAnalysisInfo(
      const Function &Fn,
      const TargetLibraryInfo &TLI_,
      std::shared_ptr<NonEscapingFuncsMap> NonEscapingFuncs_ = nullptr,
      std::shared_ptr<IPABottomTopMap> IPAFuncEscInfo = nullptr,
      std::shared_ptr<IPAArgEscFromCallsMap> IPAArgEscFromCallers_ = nullptr);

  void print(raw_ostream &OS) const;

  /// Recursively search in the instruction for the underlying objects which
  /// may escape.
  ///
  /// \p IsComplete, when given, reports whether the returned list actually
  /// accounts for everything \p V may point to. It does not when an
  /// unidentifiable object is reached, and an empty list then means "we do not
  /// know", not "nothing escapes" -- a distinction every caller has to make,
  /// because getting it wrong drops instrumentation.
  static SmallVector<UnderlObjTy> getUnderlyingMayEscObjs(
      const Value *V, const TargetLibraryInfo &TLI,
      unsigned MaxLookup = MaxUnderlObjLookup,
      std::shared_ptr<IPABottomTopMap> IPAFuncEscInfo = nullptr,
      bool *IsComplete = nullptr);

  /// Is Value V is escaping somewhere in the function
  bool isEscapedForFunc(const ObjAndPath &OAP,
                        EscReasonTy *EscReason = nullptr) const;

  EscReasonTy findObjInBBEscState(const BasicBlock *BB,
                                  const ObjAndPath &OAP) const;

  /// The escape reason for \p OAP anywhere in \p F: the union of every

  /// block's state. Serves -tsan-ea-flow-insensitive. Built once per

  /// function on first use.

  EscReasonTy findObjInFuncEscState(const Function *F,

              const ObjAndPath &OAP) const;

  /// Return escape reason for V in BB
  EscReasonTy getFullEscReasonForBB(const BasicBlock *BB,
                                    const ObjAndPath &OAP) const;

  /// Is Value V is escaping in some path from Entry to BB?
  bool isEscapedForBBImpl(const BasicBlock *BB, const ObjAndPath &OAP,
                          EscReasonTy *EscReason, bool UseIPA) const;
  bool isEscapedForBB(const BasicBlock *BB, const ObjAndPath &OAP,
                      EscReasonTy *EscReason = nullptr) const;
  bool isEscapedForBBIPA(const BasicBlock *BB, const ObjAndPath &OAP,
                         EscReasonTy *EscReason = nullptr) const;
  /// Like isEscapedForBBIPA, but for the whole function: true if \p OAP is
  /// escaped in any block of \p F. The per-object notion of escape.
  bool isEscapedInFuncIPA(const Function *F, const ObjAndPath &OAP,
                          EscReasonTy *EscReason = nullptr) const;

  /// Make action for each pointee, if given object points to something
  void
  forEachPointeeDo(const ObjAndPath &OAP, const BasicBlock *BB,
                   const std::function<void(const ObjAndPath &)> &Action) const;

private:
  // Types of object escaping states
  enum class EscKindTy { NO_ESCAPE, MAY_ESCAPE, MAY_ALIASING };
  static constexpr unsigned MaxUnderlObjLookup = 20;

  // Reference to the function being analyzed.
  const Function &AnalyzedFunc;

  // List of escaping objects corresponding to each path in the object
  using EscapedObjectsTy = std::map<ObjAndPath, EscReasonTy>;

  // IPA information about arguments escapes (bottom-top)
  std::shared_ptr<IPABottomTopMap> IPABottomTopInfo;

  // IPA information about arguments escapes from calls (top-bottom)
  std::shared_ptr<IPAArgEscFromCallsMap> IPATopDownInfo;

  // Whether return value is escaping or not (need it in IPA)
  bool IsRetEscape = false;
  struct EscapeState;

  /// Map of basic blocks to their escape analysis states.
  DenseMap<const BasicBlock *, EscapeState> BBEscapeStates;
  /// Per-function union of BBEscapeStates, filled lazily; see
  /// findObjInFuncEscState.
  mutable DenseMap<const Function *, EscapeState> FuncEscapeStates;

  /// List of functions whose arguments don't escape
  std::shared_ptr<NonEscapingFuncsMap> NonEscapingFuncs;

  const TargetLibraryInfo &TLI;

  class PointsToRelTy {
    using PointeeListTy = SmallSet<ObjAndPath, 4>;
    using PathToPointeeMap = std::map<FieldPathTy, PointeeListTy>;
    DenseMap<const Value *, PathToPointeeMap> PointsToMap;

  public:
    /// Merge with other PointsToRel object (needed in basic data flow analysis)
    void merge(const PointsToRelTy &Other);

    /// Add an points-to relation between two objects.
    void addPointsToPair(const ObjAndPath &Pointer, const ObjAndPath &Pointee);

    /// Check whether points-to relation contains Pointer-Pointee pair
    bool containsPointsToPair(const ObjAndPath &Pointer,
                              const ObjAndPath &Pointee);

    /// Traverse the (implicit) tree of aliases and get the list of aliases
    std::optional<PointeeListTy> getPointees(const ObjAndPath &Pointer) const;

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
    void addEscObj(const ObjAndPath &EscObj, EscReasonTy EscReason,
                   const Instruction *I);

    /// Adds an object to the list of escaped objects with a specified escape
    /// reason. If the object is already in the list, update escape reason.
    void addEscObjOrReason(const ObjAndPath &OAP, EscReasonTy EscReason,
                           const Instruction *I);

    /// Check CheckedObj escape status (as [maybe] external object)
    /// and update AffectedObj if needed
    void checkAndUpdEscStatus(const ObjAndPath &Pointer,
                              const ObjAndPath &Pointee,
                              const EscapeAnalysisInfo *EAI,
                              const Instruction *I);

    void forEachPointeeDo(
        const ObjAndPath &OAP,
        const std::function<void(const ObjAndPath &)> &Action) const;

    /// Adds an alias relationship between a given alias and a pointee value
    /// in the escape analysis information. If the pointee value has previously
    /// escaped or if the alias itself is an escaping pointer, the alias is
    /// also marked as escaping.
    void addPointsTo(const UnderlObjTy &Pointer, const UnderlObjTy &Pointee,
                     const EscapeAnalysisInfo *EAI, const Instruction *I);

    void merge(const EscapeState &OtherES, const EscapeAnalysisInfo *EAI) {
      PointsTo.merge(OtherES.PointsTo);
      mergeEscapedObjects(OtherES);
    }

    const EscapedObjectsTy &getEscObjs() const { return EscapedObjs; };
    const PointsToRelTy &getPointsTo() const { return PointsTo; }

    /// Try to find object in the EscapedObjects and return escape reason
    EscReasonTy getEscReason(const ObjAndPath &OAP) const;

  private:
    // Set of allocations that escape in this block.
    EscapedObjectsTy EscapedObjs;

    // map from Alloca aliases to the original Allocas
    // Note that a Value may be the alias of multiple Allocas
    PointsToRelTy PointsTo;

    /// Merge lists of escaped objects for two escape states (BBs)
    void mergeEscapedObjects(const EscapeState &OtherES);
  };

  /// Check if function returns escaped object, and update function return
  /// escape status
  void updRetEscStatus(const EscapeState &ES, const BasicBlock *BB,
                       const SmallVectorImpl<UnderlObjTy> &UnderlObjs);
  void addEscapedPtrArgs(EscapeState &ES) const;

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
  EscReasonTy getExtObjStatusIPA(const Value *V) const;

  /// Determine what kind of escape behaviour V may exhibit.
  struct EscInfoTy {
    EscKindTy EscKind;
    std::optional<std::variant<EscReasonTy, SmallVector<UnderlObjTy>>>
        EscDetails = std::nullopt;
  };

  /// Determine what kind of escape behaviour V may exhibit, return
  /// escape reason and list of aliases if applicable.
  EscInfoTy getEscInfoForOpnd(const Use &U) const;

  /// Functions to process operand/instruction pair to get escape status
  EscInfoTy getEscInfoCall(const Use &U, const Instruction *I) const;
  EscInfoTy getEscInfoLoad(const Instruction *I) const;
  EscInfoTy getEscInfoStore(const Use &U, const Instruction *I) const;
  EscInfoTy getEscInfoAtomicRMW(const Use &U, const Instruction *I) const;
  EscInfoTy getEscInfoAtomicCmpXchg(const Use &U, const Instruction *I) const;
  EscInfoTy getEscInfoGetElementPtr(const Instruction *I) const;
  EscInfoTy getEscInfoICmp(const Use &U, const Instruction *I) const;
  EscInfoTy getEscInfoRet(const Use &U) const;

  /// Print escaped objects in some path from Entry to BB
  void printEscapingForBB(const BasicBlock *BB, raw_ostream &OS) const;

  /// Taken from CaptureTracker
  static bool isDereferenceableOrNull(const Value *O, const DataLayout &DL);

  /// Check whether type contains pointers
  /// True if \p Ty is a pointer or an aggregate (struct, array or vector)
  /// that transitively contains one.
  static bool typeContainsPointerType(const Type *Ty);

  /// Find in ArgsEscapes given argument and return escape status
  EscReasonTy getArgEscBottomTopIPA(unsigned ArgNo, const Function *Func) const;

  /// Find argument in the from-callers (top-bottom) escape info
  EscReasonTy getArgEscTopDownIPA(unsigned ArgNo, const Function *Func) const;
};

/// Interface to access safety global (interprocedural) analysis results.
class EscapeAnalysisGlobalInfo {
  Module &M;
  DenseMap<const Function *, EscapeAnalysisInfo> FuncEscapeInfo;
  ModuleAnalysisManager &MAM;

  /// List of functions whose arguments don't escape
  static constexpr auto FuncWhiteListFileName = "ea_summary.txt";
  std::shared_ptr<EscapeAnalysisInfo::NonEscapingFuncsMap> NonEscapingFuncs =
      std::make_shared<EscapeAnalysisInfo::NonEscapingFuncsMap>();

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
  bool
  traverseCGBottomTop(CallGraph &CG,
                      const SmallPtrSetImpl<const Function *> &RecursiveFuncs,
                      SmallVector<std::vector<CallGraphNode *>> &SCCList);

  /// Init escape status of arguments and return
  static void
  setAllPtrArgsNotEscaped(EscapeAnalysisInfo::IPABottomTopMap &IPAFuncEscInfo,
                          const Function *F);

  /// Return map: Function -> list of call instructions
  DenseMap<const Function *, SmallVector<const CallBase *>>
  getFuncToCallSitesMap();

  /// Check weather function passed to the Objective C selector
  bool isFuncPassedToObjCSelector(const Function *F) const;

  /// Compute escape status for the function argument based on call instructions
  void traverseCGTopDown(
      const SmallVectorImpl<std::vector<CallGraphNode *>> &SCCList,
      const DenseMap<const Function *, SmallVector<const CallBase *>>
          &FuncCallSites,
      const SmallPtrSetImpl<const Function *> &RecursiveFuncs);

  void printArgEscStatus() const;

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
  static void printSCC(const std::vector<CallGraphNode *> &SCC);

  /// Read function names whose arguments don't escape
  void readNonEscapingFuncs();

  /// Write information on program (non-library) functions whose arguments don't
  /// escape, based on the IPA analysis
  void writeIPASummary();

public:
  explicit EscapeAnalysisGlobalInfo(CallGraph &CG, Module &M,
                                    ModuleAnalysisManager &MAM_);
  void print(Module &M, raw_ostream &O) const;

  /// For given pointer, get underlying objects, and get escape status for them
  bool isEscapedUndrlObjOrPointee(const Value *Addr,
                                  const TargetLibraryInfo &TLI,
                                  const BasicBlock *BB,
                                  EscapeAnalysisInfo::EscReasonTy &EscReason);
  /// isEscapedUndrlObjOrPointee with the per-object notion: does the object
  /// (or, if \p Addr was loaded, anything it may point to) escape anywhere in
  /// \p F. Used to attribute what the per-point notion elides beyond it.
  bool isEscapedUndrlObjOrPointeeAnywhere(
      const Value *Addr, const TargetLibraryInfo &TLI, const Function *F,
      EscapeAnalysisInfo::EscReasonTy &EscReason);

  /// Is Value V is escaping in some path from Entry to BB in the function F
  bool isEscapedForBBTSan(const Function *F, const BasicBlock *BB,
                          const UnderlObjTy &UnderlObj,
                          EscapeAnalysisInfo::EscReasonTy &EscReason);

  /// This is needed for using with OuterAnalysisManagerProxy
  bool invalidate(Module &, const PreservedAnalyses &,
                  ModuleAnalysisManager::Invalidator &) { return false; }

  static std::ofstream EscFuncsFile;
};

/// EscapeAnalysisInfo wrapper for the new pass manager.
class EscapeAnalysis : public AnalysisInfoMixin<EscapeAnalysis> {
  friend AnalysisInfoMixin<EscapeAnalysis>;
  static AnalysisKey Key;

public:
  using Result = EscapeAnalysisInfo;
  static Result run(Function &F, FunctionAnalysisManager &AM);
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
