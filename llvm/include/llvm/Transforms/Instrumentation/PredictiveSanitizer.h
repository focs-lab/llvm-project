//===- Transforms/Instrumentation/PredictiveSanitizer.h - PSan Pass -----------===//
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

#ifndef LLVM_TRANSFORMS_INSTRUMENTATION_PREDICTIVESANITIZER_H
#define LLVM_TRANSFORMS_INSTRUMENTATION_PREDICTIVESANITIZER_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class Function;
class Module;

/// A function pass for psan instrumentation.
///
/// Instruments functions to detect race conditions reads. This function pass
/// inserts calls to runtime library functions. If the functions aren't declared
/// yet, the pass inserts the declarations. Otherwise the existing globals are
struct PredictiveSanitizerPass : public PassInfoMixin<PredictiveSanitizerPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);
  static bool isRequired() { return true; }
};

/// A module pass for psan instrumentation.
///
/// Create ctor and init functions.
struct ModulePredictiveSanitizerPass
  : public PassInfoMixin<ModulePredictiveSanitizerPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  static bool isRequired() { return true; }
};

} // namespace llvm
#endif /* LLVM_TRANSFORMS_INSTRUMENTATION_PREDICTIVESANITIZER_H */
