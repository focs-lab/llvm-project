//===- Transforms/Instrumentation/ThreadSanitizer.h - TSan Pass -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the thread sanitizer pass.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_THREADSANITIZER_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_THREADSANITIZER_H

#include <llvm/Analysis/EscapeAnalysis.h>

#include "llvm/IR/PassManager.h"

#include <llvm/Analysis/TargetLibraryInfo.h>

namespace llvm {
class Function;
class Module;

/// A function pass for tsan instrumentation.
///
/// Instruments functions to detect race conditions reads. This function pass
/// inserts calls to runtime library functions. If the functions aren't declared
/// yet, the pass inserts the declarations. Otherwise the existing globals are
struct ThreadSanitizerPass : public PassInfoMixin<ThreadSanitizerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
  static bool isRequired() { return true; }
};

/// A module pass for tsan instrumentation.
///
/// Create ctor and init functions.
class SyncFreeInfo;
struct ModuleThreadSanitizerPass
  : public PassInfoMixin<ModuleThreadSanitizerPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
  static std::unique_ptr<SyncFreeInfo> SFI;
};

//===----------------------------------------------------------------------===//
// Sync-free analysis (needed by eliminateDominatingInstr)
//===----------------------------------------------------------------------===//

// Analysis pass to determine if functions are "dangerous" for TSan.
// A function is dangerous if it contains TSan-dangerous instructions
// or calls other dangerous functions.
class SyncFreeInfo {
public:
  explicit SyncFreeInfo(Module &M_, CallGraph &CG_,
                        AnalysisManager<Function> &AM_);
  bool invalidate(Module &, const PreservedAnalyses &,
                  ModuleAnalysisManager::Invalidator &) { return false; }
  /// True only if \p F is *known* not to synchronize. A function we have no
  /// body for is not known to be anything, so it answers false.
  bool isSyncFree(const Function *F) const {
    const auto It = IsFuncDangerousGlobal.find(F);
    return It != IsFuncDangerousGlobal.end() && !It->second;
  }

  /// True only if \p F is *known* to terminate, in the sense of containing no
  /// loops and calling nothing that does. Post-dominance elimination relies on
  /// the covering access actually being reached, so an unknown answer must
  /// read as "may not terminate".
  ///
  /// Note the name: the map records whether a function *contains* loops, and
  /// this asks the opposite question. It was previously called
  /// isContainsLoops(), which read as the negation of what it returns and was
  /// used accordingly at its only call site.
  bool isLoopFree(const Function *F) const {
    const auto It = IsFuncContainsLoops.find(F);
    return It != IsFuncContainsLoops.end() && !It->second;
  }

private:
  void findFunsContainsLoops();
  SmallDenseMap<const Function *, bool, 8> IsFuncDangerousGlobal;
  SmallDenseMap<const Function *, bool, 8> IsFuncContainsLoops;
  const Module &M;
  const CallGraph &CG;
  AnalysisManager<Function> &AM;
};

} // namespace llvm
#endif /* LLVM_TRANSFORMS_INSTRUMENTATION_THREADSANITIZER_H */
