//==- SingleThreaded.cpp --==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements single-threaded/multiple threaded analysis
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/SingleThreaded.h"
#include "llvm/IR/PassManager.h"

using namespace llvm;

void SingleThreadedInfo::identifyThreadCreators() {
  for (const std::string &CreatorName : KnownThreadCreators) {
    Function *CreatorFunc = M.getFunction(CreatorName);
    if (CreatorFunc) {
      ThreadCreatorFunctions.insert(CreatorFunc);
      errs() << "Found potential thread creator: " << CreatorName << "\n";
    }
  }

  for (const auto &[F, CGN] : CG) {
    if (!F || F->isDeclaration())
      continue;

    for (auto &CallRecord : *CGN) {
      Function *Callee = CallRecord.second->getFunction();
      if (Callee && ThreadCreatorFunctions.count(Callee)) {
        dbgs() << "Found thread creator: " << F->getName() << "\n";
        ThreadCreatorFunctions.insert(F);
        IsSingleThreadedFunc[F] = false;
        break;
      }
    }
  }
}

SingleThreadedInfo::SingleThreadedInfo(CallGraph &CG_, Module &MM_)
    : M(MM_), CG(CG_) {
  // Find all functions which create threads
  identifyThreadCreators();

  // Check whether main exists
  Function *MainFunc = M.getFunction("main");
  if (!MainFunc) {
    errs() << "WARNING: 'main' function not found in module " << M.getName()
           << ". Cannot perform single-threaded analysis.\n";
    return;
  }

  // Initialize all functions as single-threaded
  // for (const auto &F : M)
  //   IsSingleThreadedFunc[&F] = false;

  // for (const auto &[F, CGN] : CG) {
  //   if (F)
  //     IsSingleThreadedResult[F] = false;
  // }

  // Main traversal
  SmallVector<Function *, 16> WorkList;
  DenseSet<Function *> Visited;
  WorkList.push_back(MainFunc);
  IsSingleThreadedFunc[MainFunc] = true;

  while (!WorkList.empty()) {
    Function *CallerFunc = WorkList.pop_back_val();
    // if (Visited.contains(CallerFunc))
    //   continue;
    // Visited.insert(CallerFunc);

    dbgs() << "\nCaller: " << CallerFunc->getName() << "\n";

    // Current function thread creator?
    // if (ThreadCreatorFunctions.contains(CallerFunc) && CallerFunc != MainFunc) {
    //   // This function creates a thread; it and everything reachable
    //   // via this path cannot be considered single-threaded.
    //   IsSingleThreadedFunc[CallerFunc] = false;
    //   // continue; // Stop propagating this path
    // }

    // Get the CallGraphNode for the current function
    CallGraphNode *CallerNode = CG[CallerFunc];
    if (!CallerNode)
      // Function might not be in the call graph if, e.g.,
      // it's never called or its address is taken, but the call
      // is indirect and not resolved by the static CG. Skip.
      continue;

    for (const auto &[CallSite, CalleeNode] : *CallerNode) {
      Function *CalleeFunc = CalleeNode->getFunction();
      if (!CalleeFunc)
        continue;

      if (CalleeFunc->hasName())
        dbgs() << "\tCallee: " << CalleeFunc->getName() << "\n";

      WorkList.push_back(CalleeFunc);

      // Check if CalleeFunc exists in map and apply AND operation, otherwise
      // inherit
      if (const auto It = IsSingleThreadedFunc.find(CalleeFunc);
          It != IsSingleThreadedFunc.end()) {
        IsSingleThreadedFunc[CalleeFunc] =
            It->second && IsSingleThreadedFunc[CallerFunc];
      } else {
        IsSingleThreadedFunc[CalleeFunc] = IsSingleThreadedFunc[CallerFunc];
      }
    }

    // Flag if Caller calls a creator
    // bool callerBecameMultiThreaded = false;
  }

  // Check for any functions in callgraph that weren't assigned a status
  for (const auto &[F, CGN] : CG) {
    if (F && !IsSingleThreadedFunc.contains(F)) {
      // Function exists in callgraph but wasn't assigned - conservatively treat
      // it as multithreaded
      IsSingleThreadedFunc[F] = false;
    }
  }
}

void SingleThreadedInfo::print(raw_ostream &OS) const {
  OS << "\n************************************\n"
     << "***     Single-Threaded Funcs    ***\n"
     << "************************************\n\n";
  for (const auto &[Func, IsSingleThreaded] : IsSingleThreadedFunc)
    if (!Func->isDeclaration() && Func->hasName() && IsSingleThreaded)
      OS << "  " << Func->getName() << "\n";

  OS << "\n************************************\n"
     << "***    Multiple-Threaded Funcs   ***\n"
     << "************************************\n\n";
  for (const auto &[Func, IsSingleThreaded] : IsSingleThreadedFunc)
    if (!Func->isDeclaration() && Func->hasName() && !IsSingleThreaded)
      OS << "  " << Func->getName() << "\n";

  OS << "\n************************************\n"
     << "***    Thread Creator Functions   ***\n"
     << "************************************\n\n";
  for (const Function *F : ThreadCreatorFunctions)
    if (F->hasName())
      OS << "  " << F->getName() << "\n";
}

AnalysisKey SingleThreaded::Key;

SingleThreaded::Result SingleThreaded::run(Module &M,
                                           ModuleAnalysisManager &AM) {
  return SingleThreadedInfo(AM.getResult<CallGraphAnalysis>(M), M);
}

PreservedAnalyses
SingleThreadedPrinterPass::run(Module &M, ModuleAnalysisManager &AM) const {
  OS << "Printing analysis 'Single Threaded' for module '" << M.getName()
     << "':\n";
  AM.getResult<SingleThreaded>(M).print(OS);
  return PreservedAnalyses::all();
}

#define DEBUG_TYPE "ownership"
