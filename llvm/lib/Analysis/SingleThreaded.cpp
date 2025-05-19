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
#include <fstream>

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
  return It != FuncType.end() && It->second == FuncContext::SingleThreaded;
}

void SingleThreadedInfo::identifyBaseThreadCreators() {
  for (const auto CreatorName : KnownThreadCreators) {
    Function *CreatorFunc = M.getFunction(CreatorName);
    if (CreatorFunc) {
      FuncType[CreatorFunc] = FuncContext::ThreadCreator;
      errs() << "Found potential thread creator: " << CreatorName << "\n";
    }
  }
}

void SingleThreadedInfo::markFuncAndAllCalleesAsMultithreaded(
    const Function *CallerFunc, const CallGraphNode &CGN,
    FuncTypeMap &FuncTypeNew) {
  const auto FuncTypeIt = FuncTypeNew.find(CallerFunc);
  if ((FuncTypeIt != FuncTypeNew.end()) &&
      ((FuncTypeIt->second == FuncContext::MultiThreaded) ||
       (FuncTypeIt->second == FuncContext::ThreadCreator)))
    return;

  FuncTypeNew[CallerFunc] = FuncContext::MultiThreaded;

  for (const auto &[CallSite2, CalleeCGN] : CGN) {
    const Function *CalleeFunc = CalleeCGN->getFunction();

    if (needToSkipFunc(CalleeFunc) || (CallerFunc == CalleeFunc))
      continue;

    markFuncAndAllCalleesAsMultithreaded(CalleeFunc, *CalleeCGN, FuncTypeNew);
    LLVM_DEBUG(if (CalleeFunc->hasName()) dbgs()
               << "\tMarking function " << CalleeFunc->getName()
               << " as multi-threaded\n");
  }
}

bool SingleThreadedInfo::runSTMTAnalysis() {
  // Locate main() function as entry point for analysis
  MainFunc = M.getFunction("main");
  if (!MainFunc) {
    errs() << "WARNING: 'main' function not found in module " << M.getName()
           << ". Cannot perform single-threaded analysis.\n";
    return true;
  }

  FuncType[MainFunc] = FuncContext::SingleThreaded;
  FuncTypeMap FuncTypeNew(FuncType);

  // Iteratively propagate thread creation information through call graph
  // Continue until no new thread creators are identified (fixed point reached)
  do {
    FuncType = FuncTypeNew;

    // Examine each function in the call graph to identify thread creators
    // and propagate thread creation status to their callees
    for (const auto &[F, CGN] : *CG) {
      if (needToSkipFunc(F))
        continue;

      // Check if the function is used in indirect calls
      // Find and mark functions whose addresses are taken as multi-threaded
      if (F->hasAddressTaken()) { // && FuncTypeNew[F] == "ST"
        LLVM_DEBUG(dbgs() << "Function " << F->getName()
                   << " has its address taken - marking as multi-threaded\n");
        markFuncAndAllCalleesAsMultithreaded(F, *CGN, FuncTypeNew);
      } else {
        const auto FuncTypeIt = FuncTypeNew.find(F);
        if (FuncTypeIt == FuncTypeNew.end())
          FuncTypeNew[F] = FuncContext::SingleThreaded;
      }

      LLVM_DEBUG(if (F->hasName()) {
        dbgs() << "\nFunction: " << F->getName();
        auto It = FuncTypeNew.find(F);
        if (It != FuncTypeNew.end())
          dbgs() << "\t(" << toString(It->second) << ")";
        dbgs() << "\n";
      });

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
          // We reached this function, mark as single-threaded
          FuncTypeNew[Callee] = FuncContext::SingleThreaded;
          continue;
        }

        if (FuncTypeIt->second == FuncContext::ThreadCreator) {
          LLVM_DEBUG(dbgs() << "\tMarking function " << F->getName()
                            << " as thread creator\n");
          FuncTypeNew[F] = FuncContext::ThreadCreator;

          // Mark all callees of F as multithreaded
          markFuncAndAllCalleesAsMultithreaded(F, *CGN, FuncTypeNew);
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
  } while (FuncTypeNew != FuncType);
  return false;
}

SingleThreadedInfo::SingleThreadedInfo(CallGraph &CG_, Module &MM_)
    : M(MM_), CG(&CG_) {
  LLVM_DEBUG(dbgs() << "\n=== Single Threaded Analysis ===\n");

  // Find all functions which create threads
  identifyBaseThreadCreators();

  // Run STMT analysis to identify single-threaded functions
  runSTMTAnalysis();

  // Run SWMR analysis
  findReadOnlyGlobals();

  writeSummary();
}

//===----------------------------------------------------------------------===//
// SWMR analysis
//===----------------------------------------------------------------------===//

void SingleThreadedInfo::findReadOnlyGlobals() {
  LLVM_DEBUG(dbgs() << "\n=== SWMR Analysis ===\n");

  // For each global, check if it's only read in multithreaded functions
  for (const auto &GV : M.globals()) {
    // Skip constant global variables
    if (GV.isConstant())
      continue;

    LLVM_DEBUG(dbgs() << "Checking global: " << GV.getName() << "\n");
    bool IsWritten = false;
    bool IsRead = false;

    // Check all uses of this global
    for (const User *U : GV.users()) {
      if (const auto *I = dyn_cast<Instruction>(U)) {
        // Skip if the function is single-threaded
        if (isSingleThreaded(I->getFunction()))
          continue;

        // For multithreaded functions, check if this is a write operation
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
  if (!ReadFromSummary) {
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
  }

  OS << "\n============================================\n"
     << "           Single-Threaded Functions          \n"
     << "============================================\n\n";
  for (const auto &[Func, Context] : FuncType)
    if (!Func->isDeclaration() && Func->hasName() &&
        Context == FuncContext::SingleThreaded)
      OS << "  " << Func->getName() << "\n";

  if (!ReadFromSummary) {
    OS << "\n============================================\n"
       << "            Unclassified Functions           \n"
       << "============================================\n\n";
    for (const auto &[F, CGN] : *CG)
      if (F && !F->isDeclaration() && F->hasName() && !FuncType.contains(F))
        OS << "  " << F->getName() << "\n";
  }

  OS << "\n============================================\n"
     << "              Read-Only Globals              \n"
     << "============================================\n\n";
  for (const GlobalVariable *GV : ReadOnlyGlobals)
    OS << "  " << GV->getName() << "\n";
}

void SingleThreadedInfo::writeSummary() const {
  std::ofstream Summary(SingleThreadedSummaryFileName);
  if (!Summary.is_open()) {
    errs() << "Error: Could not open file " << SingleThreadedSummaryFileName
           << " for writing\n";
    return;
  }
  LLVM_DEBUG(dbgs() << "Writing analysis results to "
                    << SingleThreadedSummaryFileName << "\n");

  Summary << SummaryHeaderST << "\n";
  for (const auto &[Func, Context] : FuncType)
    if (!Func->isDeclaration() && Func->hasName() &&
        Context == FuncContext::SingleThreaded)
      Summary << Func->getName().str() << "\n";
  Summary << "\n";

  Summary << SummaryHeaderSWMR << "\n";
  for (const GlobalVariable *GV : ReadOnlyGlobals)
    Summary << GV->getName().str() << "\n";

  Summary.close();
}

void SingleThreadedInfo::readSummary() {
  // Clear existing analysis results
  FuncType.clear();
  ReadOnlyGlobals.clear();

  std::ifstream Summary(SingleThreadedSummaryFileName);
  if (!Summary.is_open()) {
    errs() << "Error: Could not open file " << SingleThreadedSummaryFileName
           << " for reading\n";
    return;
  }
  LLVM_DEBUG(dbgs() << "Reading analysis results from "
                    << SingleThreadedSummaryFileName << "\n");

  std::string Line;
  bool ReadingST = false;
  bool ReadingSWMR = false;

  while (std::getline(Summary, Line)) {
    if (Line == SummaryHeaderST) {
      ReadingST = true;
      ReadingSWMR = false;
    } else if (Line == SummaryHeaderSWMR) {
      ReadingST = false;
      ReadingSWMR = true;
    } else if (Line.empty()) {
      ReadingST = false;
      ReadingSWMR = false;
      continue;
    }

    if (ReadingST) {
      if (const Function *F = M.getFunction(Line))
        FuncType[F] = FuncContext::SingleThreaded;
    } else if (ReadingSWMR) {
      if (const GlobalVariable *GV = M.getGlobalVariable(Line))
        ReadOnlyGlobals.insert(GV);
    }
  }

  Summary.close();
}

AnalysisKey SingleThreaded::Key;

SingleThreaded::Result SingleThreaded::run(Module &M,
                                           ModuleAnalysisManager &AM) {
  if (std::ifstream SummaryFile(SingleThreadedSummaryFileName);
      SummaryFile.good()) {
    LLVM_DEBUG(dbgs() << "Found existing summary file. Loading results.\n");
    SummaryFile.close();
    return SingleThreadedInfo(M);
  }
  LLVM_DEBUG(dbgs() << "No summary file found. Running full analysis.\n");
  return SingleThreadedInfo(AM.getResult<CallGraphAnalysis>(M), M);
}

PreservedAnalyses
SingleThreadedPrinterPass::run(Module &M, ModuleAnalysisManager &AM) const {
  OS << "Printing analysis 'Single Threaded' for module '" << M.getName()
     << "':\n";
  AM.getResult<SingleThreaded>(M).print(OS);
  return PreservedAnalyses::all();
}