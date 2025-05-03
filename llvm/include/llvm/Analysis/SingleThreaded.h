//==- LockOwnership.h - --==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the generic Lock Ownership interface.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_SINGLETHREADED_H
#define LLVM_ANALYSIS_SINGLETHREADED_H

#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/PassManager.h"

#include <set>

namespace llvm {

// Add names of known thread creation functions here
const std::set<std::string> KnownThreadCreators = {"pthread_create"};

/// Interface to access safety global (interprocedural) analysis results.
class SingleThreadedInfo {
public:
  explicit SingleThreadedInfo(CallGraph &CG_, Module &M);
  void print(raw_ostream &O) const;

  /// This is needed for using with OuterAnalysisManagerProxy
  bool invalidate(Module &, const PreservedAnalyses &,
                  ModuleAnalysisManager::Invalidator &) {
    return false;
  }

  /// Returns true if the given function is executed in a multithreaded context
  /// or creates threads
  // bool isMultithreaded(const Function *F) const;

  /// Returns true if the given function is executed in a single-threaded
  /// context and doesn't create threads
  bool isSingleThreaded(const Function *F) const;

  /// Returns true if the given global variable is only read (not written to) in
  /// multithreaded functions
  bool isReadOnly(const GlobalVariable *GV) const {
    return ReadOnlyGlobals.contains(GV);
  }

private:
  Module &M;
  CallGraph &CG;

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

  /// Find all base functions-thread creators
  void identifyBaseThreadCreators();

  /// Identifies global variables that are only read (not written to) in
  /// multithreaded functions This analysis helps identify global variables that
  /// can be safely accessed concurrently without synchronization in
  /// multithreaded contexts, since they are never modified.
  void findReadOnlyGlobals();
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

#endif // LLVM_ANALYSIS_LOCKOWNERSHIP_H
