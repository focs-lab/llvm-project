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

#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

using namespace llvm;

#define DEBUG_TYPE "single-threaded"

const char *SingleThreadedInfo::toString(FuncContext FC) {
  switch (FC) {
  case FuncContext::ThreadCreator:
    return "ThreadCreator";
  case FuncContext::MultiThreaded:
    return "MultiThreaded";
  case FuncContext::SingleThreaded:
    return "SingleThreaded";
  }
  return "Unknown";
}

bool SingleThreadedInfo::isSingleThreaded(const Function *F) const {
  const auto It = FuncType.find(F);
  return It != FuncType.end() && It->second != FuncContext::ThreadCreator &&
         It->second != FuncContext::MultiThreaded;
}

void SingleThreadedInfo::identifyBaseThreadCreators() {
  for (const std::string &CreatorName : KnownThreadCreators) {
    Function *CreatorFunc = M.getFunction(CreatorName);
    if (CreatorFunc) {
      FuncType[CreatorFunc] = FuncContext::ThreadCreator;
      errs() << "Found potential thread creator: " << CreatorName << "\n";
    }
  }
}

SingleThreadedInfo::SingleThreadedInfo(CallGraph &CG_, Module &MM_)
    : M(MM_), CG(CG_) {
  LLVM_DEBUG(dbgs() << "\n=== Single Threaded Analysis ===\n");

  // Find all functions which create threads
  identifyBaseThreadCreators();

  // Check whether main exists
  Function *MainFunc = M.getFunction("main");
  if (!MainFunc) {
    errs() << "WARNING: 'main' function not found in module " << M.getName()
           << ". Cannot perform single-threaded analysis.\n";
    return;
  }

  FuncType[MainFunc] = FuncContext::SingleThreaded;
  FuncTypeMap FuncTypeNew(FuncType);

  // Iteratively propagate thread creation information through call graph
  // Continue until no new thread creators are identified (fixed point reached)
  do {
    FuncType = FuncTypeNew;

    // Examine each function in the call graph to identify thread creators
    // and propagate thread creation status to their callees
    for (const auto &[F, CGN] : CG) {
      if (!F || F == MainFunc)
        continue;

      const auto FuncTypeIt = FuncTypeNew.find(F);
      if (FuncTypeIt == FuncTypeNew.end()) {
        // Check if function matches pthread_create thread function signature
        // void* (*)(void*) or equivalent
        if (F->getFunctionType()->getNumParams() == 1 &&
            F->getFunctionType()->getReturnType()->isPointerTy() &&
            F->getFunctionType()->getParamType(0)->isPointerTy()) {
          FuncTypeNew[F] = FuncContext::MultiThreaded;
        } else {
          FuncTypeNew[F] = FuncContext::SingleThreaded;
        }
      }

      LLVM_DEBUG(if (F->hasName()) dbgs()
                     << "\nFunction: " << F->getName() << "\n";);

      // Examine each callee to check if it's a thread creator. If so, mark the
      // current function and its callees as multithreaded
      for (const auto &[CallSite, CalleeNode] : *CGN) {
        const Function *Callee = CalleeNode->getFunction();
        if (!Callee || Callee == MainFunc)
          continue;

        LLVM_DEBUG(if (Callee->hasName()) dbgs()
                   << "\tCallee: " << Callee->getName() << "\n");

        const auto FuncTypeIt = FuncTypeNew.find(Callee);
        if (FuncTypeIt == FuncTypeNew.end()) {
          FuncType[Callee] = FuncContext::SingleThreaded;
          continue;
        }

        if (FuncTypeIt->second == FuncContext::ThreadCreator) {
          LLVM_DEBUG(dbgs() << "\tMarking function " << F->getName()
                            << " as thread creator\n");
          FuncTypeNew[F] = FuncContext::ThreadCreator;

          // Mark all callees of F as multithreaded
          for (const auto &[CallSite2, CalleeNode2] : *CGN) {
            const Function *CalleeFunc = CalleeNode2->getFunction();

            if (CalleeFunc == MainFunc)
              continue;

            const auto FuncTypeIt = FuncTypeNew.find(CalleeFunc);
            if (FuncTypeIt != FuncTypeNew.end()) {
              if (FuncTypeIt->second == FuncContext::ThreadCreator)
                // It's a stronger property than MultiThreaded, leave it as is
                continue;
            }

            FuncTypeNew[CalleeFunc] = FuncContext::MultiThreaded;
            LLVM_DEBUG(if (CalleeFunc->hasName()) dbgs()
                       << "\tMarking function " << CalleeFunc->getName()
                       << " as multi-threaded\n");
          }
          break;
        }
      }
    }

    LLVM_DEBUG(
        dbgs() << "\n----------------------------------------\n";
        dbgs() << "FuncType map contents:\n";
        for (const auto &[Func, Context] : FuncType) if (Func->hasName()) dbgs()
        << "  " << Func->getName() << ": " << toString(Context) << "\n";
        dbgs() << "----------------------------------------\n";
        dbgs() << "FuncTypeNew map contents:\n";
        for (const auto &[Func, Context] : FuncTypeNew) if (Func->hasName())
            dbgs()
        << "  " << Func->getName() << ": " << toString(Context) << "\n";
        dbgs() << "----------------------------------------\n";);
    // sleep(1);
  } while (FuncTypeNew != FuncType);

  findReadOnlyGlobals();
}

//===----------------------------------------------------------------------===//
// SWMR analysis
//===----------------------------------------------------------------------===//

void SingleThreadedInfo::findReadOnlyGlobals() {
  LLVM_DEBUG(dbgs() << "\n=== SWMR Analysis ===\n");

  // For each global, check if it's only read in multithreaded functions
  for (const auto &GV : M.globals()) {
    LLVM_DEBUG(dbgs() << "Checking global: " << GV.getName() << "\n");
    bool IsWritten = false;
    bool IsRead = false;

    // Check all uses of this global
    for (const User *U : GV.users()) {
      if (const auto *I = dyn_cast<Instruction>(U)) {
        // Skip if the function is not multithreaded
        if (isSingleThreaded(I->getFunction()))
          continue;

        // Check if this is a write operation
        if (const StoreInst *SI = dyn_cast<StoreInst>(I)) {
          if (SI->getPointerOperand() == &GV) {
            IsWritten = true;
            break;
          }
        } else {
          IsRead = true;
        }
      }
    }

    if (IsRead && !IsWritten)
      ReadOnlyGlobals.insert(&GV);
  }
}

void SingleThreadedInfo::print(raw_ostream &OS) const {
  OS << "\n============================================\n"
     << "            Thread Creator Functions          \n"
     << "============================================\n\n";
  for (const auto &[Func, Context] : FuncType)
    if (!Func->isDeclaration() && Func->hasName() &&
        Context == FuncContext::ThreadCreator)
      OS << "  " << Func->getName() << "\n";

  OS << "\n============================================\n"
     << "           Multi-Threaded Functions          \n"
     << "============================================\n\n";
  for (const auto &[Func, Context] : FuncType)
    if (!Func->isDeclaration() && Func->hasName() &&
        Context == FuncContext::MultiThreaded)
      OS << "  " << Func->getName() << "\n";

  OS << "\n============================================\n"
     << "           Single-Threaded Functions          \n"
     << "============================================\n\n";
  for (const auto &[Func, Context] : FuncType)
    if (!Func->isDeclaration() && Func->hasName() &&
        Context == FuncContext::SingleThreaded)
      OS << "  " << Func->getName() << "\n";

  OS << "\n============================================\n"
     << "            Unclassified Functions           \n"
     << "============================================\n\n";
  for (const auto &[F, CGN] : CG)
    if (F && !F->isDeclaration() && F->hasName() && !FuncType.contains(F))
      OS << "  " << F->getName() << "\n";

  OS << "\n============================================\n"
     << "              Read-Only Globals              \n"
     << "============================================\n\n";
  for (const GlobalVariable *GV : ReadOnlyGlobals)
    OS << "  " << GV->getName() << "\n";
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