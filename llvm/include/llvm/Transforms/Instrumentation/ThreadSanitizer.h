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
struct ModuleThreadSanitizerPass
  : public PassInfoMixin<ModuleThreadSanitizerPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
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
                        const TargetLibraryInfo &TLI_);
  bool invalidate(Module &, const PreservedAnalyses &,
                  ModuleAnalysisManager::Invalidator &) { return false; }
  bool isSyncFree(const Function *F) const {
    const auto It = IsFuncDangerousMap.find(F);
    return It != IsFuncDangerousMap.end() && !It->second;
  }

private:
  SmallDenseMap<const Function *, bool, 8> IsFuncDangerousMap;
  const Module &M;
  const CallGraph &CG;
  const TargetLibraryInfo &TLI;
};

class SyncFreeAnalysis : public AnalysisInfoMixin<SyncFreeAnalysis> {
  friend AnalysisInfoMixin<SyncFreeAnalysis>;
  static AnalysisKey Key;

public:
  using Result = SyncFreeInfo;
  Result run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
#endif /* LLVM_TRANSFORMS_INSTRUMENTATION_THREADSANITIZER_H */
