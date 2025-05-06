//==- SingleThreaded.h - --==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements Single-threaded/Multiple-threaded analysis
// and Single-writer/Multiple-reader analysis.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_SINGLETHREADED_H
#define LLVM_ANALYSIS_SINGLETHREADED_H

#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/PassManager.h"

#include <set>

namespace llvm {

const std::string SingleThreadedSummaryFileName = "st_summary.txt";
const std::string SummaryHeaderST = "--- Single-Threaded Functions ---";
const std::string SummaryHeaderSWMR =
    "--- Read-Only Global Variables (in Multi-Threaded Context) ---";

// Add names of known thread creation functions here
const SmallDenseSet<StringRef, 4> KnownThreadCreators = {"pthread_create"};

/// Interface to access safety global (interprocedural) analysis results.
class SingleThreadedInfo {
public:
  /// Constructs SingleThreadedInfo using provided CallGraph and Module
  /// Performs initialization and analysis of thread contexts
  explicit SingleThreadedInfo(CallGraph &CG_, Module &M);

  /// Default constructor that initializes SingleThreadedInfo by reading
  /// analysis results from a previously written summary file.
  explicit SingleThreadedInfo(Module &MM) : M(MM), ReadFromSummary(true) {
    readSummary();
  }

  void print(raw_ostream &O) const;

  /// Reads analysis results from a previously written summary file. This allows
  /// reusing previously computed analysis results for single-threaded functions
  /// and read-only globals across different compilation units
  void readSummary();

  /// This is needed for using with OuterAnalysisManagerProxy
  bool invalidate(Module &, const PreservedAnalyses &,
                  ModuleAnalysisManager::Invalidator &) {
    return false;
  }

  /// Returns true if the given function is executed in a single-threaded
  /// context and doesn't create threads
  bool isSingleThreaded(const Function *F) const;

  /// Returns true if the given function is executed in a multithreaded context
  /// or creates threads
  bool isMultithreaded(const Function *F) const { return !isSingleThreaded(F); }

  /// Returns true if the given global variable is only read (not written to) in
  /// multithreaded functions
  bool isReadOnly(const GlobalVariable *GV) const {
    return ReadOnlyGlobals.contains(GV);
  }

private:
  Module &M;
  CallGraph *CG = nullptr;
  const bool ReadFromSummary = false;
  Function *MainFunc = nullptr;

  enum class FuncContext {
    ThreadCreator, // Function that creates new threads
    MultiThreaded, // Function executed in multithreaded context
    SingleThreaded // Function executed in single-threaded context
  };

  static const char *toString(FuncContext FC);

  // Maps Function pointers to their threading context (whether they create
  // threads or are executed in a multithreaded environment)
  using FuncTypeMap = SmallDenseMap<const Function *, FuncContext>;
  FuncTypeMap FuncType;

  SmallPtrSet<const GlobalVariable *, 4> ReadOnlyGlobals;

  /// Consider only defined functions
  bool needToSkipFunc(const Function *F) {
    return !F || F->isDeclaration() || F == MainFunc;
  }

  /// Performs single-threaded/multi-threaded analysis on the module
  /// This function analyzes the call graph to identify:
  /// - Thread creator functions (functions that create new threads)
  /// - Multi-threaded functions (functions that run in multiple threads)
  /// - Single-threaded functions (functions that run in a single thread)
  ///
  /// The analysis works by:
  /// 1. Starting with main() as single-threaded
  /// 2. Identifying thread creators based on function signatures
  /// 3. Propagating thread creation information through the call graph
  /// 4. Iterating until a fixed point is reached (no new thread creators found)
  ///
  /// @return false on success, true if analysis cannot be performed
  bool runSTMTAnalysis();

  /// Find all base functions-thread creators
  void identifyBaseThreadCreators();

  /// Recursively marks all callees of a function as multi-threaded in the
  /// function type map. This is used when a function is identified as a thread
  /// creator or multi-threaded, and we need to propagate that status to all
  /// functions it can call.
  void markFuncAndAllCalleesAsMultithreaded(const Function *CallerFunc,
                                            const CallGraphNode &CGN,
                                            FuncTypeMap &FuncTypeNew);

  /// Identifies global variables that are only read (not written to) in
  /// multithreaded functions This analysis helps identify global variables that
  /// can be safely accessed concurrently without synchronization in
  /// multithreaded contexts, since they are never modified.
  void findReadOnlyGlobals();

  /// Writes analysis results to a summary file. The summary includes lists of
  /// single-threaded functions and read-only global variables that are safe
  /// in multi-threaded contexts
  void writeSummary() const;
};

/// This pass performs the global (interprocedural) escape analysis.
class SingleThreaded : public AnalysisInfoMixin<SingleThreaded> {
  friend AnalysisInfoMixin<SingleThreaded>;
  static AnalysisKey Key;

public:
  using Result = SingleThreadedInfo;
  static Result run(Module &M, ModuleAnalysisManager &AM);
};

/// Printer pass for the \c OwnershipAnalysis results.
class SingleThreadedPrinterPass
    : public PassInfoMixin<SingleThreadedPrinterPass> {
  raw_ostream &OS;

public:
  explicit SingleThreadedPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) const;
  static bool isRequired() { return true; }
};

} // end namespace llvm

#endif // LLVM_ANALYSIS_SINGLETHREADED_H
