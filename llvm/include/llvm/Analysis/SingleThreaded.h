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
const std::set<std::string> KnownThreadCreators = { "pthread_create" };

/// Interface to access safety global (interprocedural) analysis results.
class SingleThreadedInfo {
public:
  explicit SingleThreadedInfo(CallGraph &CG_, Module &M);
  void print(raw_ostream &O) const;

  /// This is needed for using with OuterAnalysisManagerProxy
  bool invalidate(Module &, const PreservedAnalyses &,
                  ModuleAnalysisManager::Invalidator &) { return false; }

private:
  Module &M;
  CallGraph &CG;

  // true, is the function is executed in single-threaded context
  DenseMap<const Function *, bool> IsSingleThreadedFunc;

  // Set of functions which create threads
  SmallPtrSet<const Function *, 4> ThreadCreatorFunctions;

  // Find all functions-thread creators
  void identifyThreadCreators();
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
