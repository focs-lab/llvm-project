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

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include <filesystem>
#include <fstream>

using namespace llvm;

#define DEBUG_TYPE "single-threaded"

cl::opt<bool> llvm::TsanUseAnalysisSummaries(
    "tsan-use-analysis-summaries", cl::init(false), cl::Hidden,
    cl::desc("Seed the TSan analyses from summaries written while compiling "
             "earlier modules. Only sound in a build that analyses every "
             "module before instrumenting any of them."));

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

void llvm::collectAccessingInstrs(const Value *V,
                                  SmallVectorImpl<const Instruction *> &Out,
                                  SmallPtrSetImpl<const Value *> &Visited) {
  if (!Visited.insert(V).second)
    return;
  for (const User *U : V->users()) {
    if (const auto *I = dyn_cast<Instruction>(U))
      Out.push_back(I);
    else if (isa<ConstantExpr>(U) || isa<GlobalAlias>(U))
      collectAccessingInstrs(U, Out, Visited);
  }
}

bool llvm::isKnownThreadCreator(const Function &F) {
  const StringRef Name = F.getName();
  if (KnownThreadCreators.contains(Name))
    return true;
  return llvm::any_of(ThreadCreatorPrefixes, [&Name](StringRef Prefix) {
    return Name.starts_with(Prefix);
  });
}

bool SingleThreadedInfo::isSingleThreaded(const Function *F) const {
  // main is answered per instruction, not per function -- see the header.
  if (F && F == MainFunc)
    return false;
  const auto It = FuncType.find(F);
  return It != FuncType.end() && It->second == FuncContext::SingleThreaded;
}

bool SingleThreadedInfo::mayCreateThread(const Instruction &I) const {
  const auto *CB = dyn_cast<CallBase>(&I);
  if (!CB)
    return false;

  const Function *Callee = CB->getCalledFunction();
  // An indirect call could reach anything, thread creation included.
  if (!Callee)
    return true;
  if (isKnownThreadCreator(*Callee))
    return true;

  // Only ThreadCreator means "may start a thread": runSTMTAnalysis propagates
  // that property up the call graph from the known creators. MultiThreaded is
  // a different statement -- that the function *runs* while other threads
  // exist -- and treating it as creation would both over-approximate and, since
  // propagateMTFromMain adds MultiThreaded marks, make this answer depend on
  // how far that propagation had got.
  const auto It = FuncType.find(Callee);
  return It != FuncType.end() && It->second == FuncContext::ThreadCreator;
}

bool SingleThreadedInfo::blockCreatesThreads(const BasicBlock &BB) const {
  return llvm::any_of(
      BB, [this](const Instruction &I) { return mayCreateThread(I); });
}

void SingleThreadedInfo::computeMainMTBlocks() {
  MTBlocksInMain.clear();
  if (!MainFunc || MainFunc->isDeclaration())
    return;

  // Everything reachable from a block that starts a thread is multi-threaded,
  // including that block itself when it sits on a cycle.
  SmallVector<const BasicBlock *, 16> Worklist;
  for (const BasicBlock &BB : *MainFunc)
    if (blockCreatesThreads(BB))
      for (const BasicBlock *Succ : successors(&BB))
        if (MTBlocksInMain.insert(Succ).second)
          Worklist.push_back(Succ);

  while (!Worklist.empty())
    for (const BasicBlock *Succ : successors(Worklist.pop_back_val()))
      if (MTBlocksInMain.insert(Succ).second)
        Worklist.push_back(Succ);
}

void SingleThreadedInfo::propagateMTFromMain() {
  if (!MainFunc || !CG || MainFunc->isDeclaration())
    return;

  for (const BasicBlock &BB : *MainFunc) {
    for (const Instruction &I : BB) {
      // Only calls made once main is multi-threaded matter.
      if (isSingleThreaded(&I))
        continue;
      const auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      const Function *Callee = CB->getCalledFunction();
      if (needToSkipFunc(Callee))
        continue;
      if (const CallGraphNode *CGN = (*CG)[Callee])
        markFuncAndAllCalleesAsMultithreaded(Callee, *CGN, FuncType);
    }
  }
}

bool SingleThreadedInfo::isSingleThreaded(const Instruction *I) const {
  if (!I)
    return false;
  const Function *F = I->getFunction();
  if (F != MainFunc)
    return isSingleThreaded(F);

  const BasicBlock *BB = I->getParent();
  if (MTBlocksInMain.contains(BB))
    return false;

  // The block is single-threaded on entry, but a thread creating call earlier
  // in it ends that.
  for (const Instruction &Prev : *BB) {
    if (&Prev == I)
      break;
    if (mayCreateThread(Prev))
      return false;
  }
  return true;
}

void SingleThreadedInfo::identifyBaseThreadCreators() {
  for (Function &F : M) {
    if (isKnownThreadCreator(F)) {
      FuncType[&F] = FuncContext::ThreadCreator;
      LLVM_DEBUG(dbgs() << "Found potential thread creator: " << F.getName()
                        << "\n");
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
    LLVM_DEBUG(dbgs() << "'main' not found in module " << M.getName()
                      << "; single-threaded analysis cannot run.\n");
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

  // Refine main down to basic-block granularity, then push that result out
  // into the functions main calls once it is multi-threaded.
  computeMainMTBlocks();
  propagateMTFromMain();

  // Run SWMR analysis
  findSWMRGlobals();

  // Only write a summary if something may read one. Writing unconditionally
  // created a tsan-logs/ directory in whatever tree the compiler ran in.
  if (TsanUseAnalysisSummaries)
    writeSummary();
}

//===----------------------------------------------------------------------===//
// SWMR analysis
//===----------------------------------------------------------------------===//

void SingleThreadedInfo::findSWMRGlobals() {
  LLVM_DEBUG(dbgs() << "\n=== SWMR Analysis ===\n");

  // For each global, check if it's only read in multithreaded functions
  for (const auto &GV : M.globals()) {
    // Skip constant global variables
    if (GV.isConstant())
      continue;

    LLVM_DEBUG(dbgs() << "Checking global: " << GV.getName() << "\n");
    bool IsWrittenInMT = false;

    SmallVector<const Instruction *, 8> Accesses;
    SmallPtrSet<const Value *, 8> VisitedUsers;
    collectAccessingInstrs(&GV, Accesses, VisitedUsers);

    for (const Instruction *I : Accesses) {
      // A plain load never writes.
      //
      // Comparing a store's pointer operand against the global itself, as this
      // used to, recognised only `g = x` for a scalar g. It missed `g[2] = x`,
      // whose pointer operand is a constant getelementptr, and anything
      // written through a derived pointer -- so an array written by every
      // thread was still reported as read-only.
      if (const auto *LI = dyn_cast<LoadInst>(I))
        if (getUnderlyingObject(LI->getPointerOperand()) == &GV)
          continue;

      if (const auto *SI = dyn_cast<StoreInst>(I)) {
        if (getUnderlyingObject(SI->getPointerOperand()) == &GV) {
          // A write that runs before any thread exists cannot make the global
          // unsafe to read concurrently.
          if (isSingleThreaded(I))
            continue;
          IsWrittenInMT = true;
          LLVM_DEBUG(dbgs() << "  written in MT context: " << *I << "\n");
          break;
        }
      }

      // Anything else hands the address somewhere we cannot follow: a call, or
      // a store of the pointer itself. The thread-context test does not apply
      // here -- passing &g to pthread_create happens while the program is
      // still single-threaded, and that is precisely the case where the write
      // arrives later, on another thread.
      IsWrittenInMT = true;
      LLVM_DEBUG(dbgs() << "  address escapes at: " << *I << "\n");
      break;
    }

    if (!IsWrittenInMT)
      SWMRGlobals.insert(&GV);
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
  // main is deliberately absent: it is classified per basic block, and
  // isSingleThreaded() answers false for it, so listing it here as
  // single-threaded described something the analysis does not believe.
  for (const auto &[Func, Context] : FuncType)
    if (!Func->isDeclaration() && Func->hasName() && Func != MainFunc &&
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
  for (const GlobalVariable *GV : SWMRGlobals)
    OS << "  " << GV->getName() << "\n";
}

const std::string LogDir = "tsan-logs";
static void createLogDir() {
  std::error_code EC;
  if (!std::filesystem::exists(LogDir))
    if (!std::filesystem::create_directory(LogDir, EC) && EC)
      errs() << "Error creating directory " << LogDir << ": " << EC.message()
             << "\n";
}

void SingleThreadedInfo::writeSummary() const {
  createLogDir();

  const auto STSummaryPath =
      SummaryDirName + "/" + SingleThreadedSummaryFileName;
  std::ofstream Summary(STSummaryPath);
  if (!Summary.is_open()) {
    errs() << "Error: Could not open " << STSummaryPath << " for writing\n";
    return;
  }
  LLVM_DEBUG(dbgs() << "Writing analysis results to "
                    << SingleThreadedSummaryFileName << "\n");

  Summary << SummaryHeaderST << "\n";
  for (const auto &[Func, Context] : FuncType)
    if (!Func->isDeclaration() && Func->hasName() && Func != MainFunc &&
        Context == FuncContext::SingleThreaded)
      Summary << Func->getName().str() << "\n";
  Summary << "\n";

  Summary << SummaryHeaderSWMR << "\n";
  for (const GlobalVariable *GV : SWMRGlobals)
    Summary << GV->getName().str() << "\n";

  Summary.close();
}

void SingleThreadedInfo::readSummary() {
  // Clear existing analysis results
  FuncType.clear();
  SWMRGlobals.clear();

  std::ifstream Summary(SummaryDirName + "/" + SingleThreadedSummaryFileName);
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
      continue;
    }
    if (Line == SummaryHeaderSWMR) {
      ReadingST = false;
      ReadingSWMR = true;
      continue;
    }
    if (Line.empty()) {
      ReadingST = false;
      ReadingSWMR = false;
      continue;
    }

    if (ReadingST) {
      if (Line == "main")
        continue; // classified per instruction, never wholesale
      if (const Function *F = M.getFunction(Line))
        FuncType[F] = FuncContext::SingleThreaded;
    } else if (ReadingSWMR) {
      if (const GlobalVariable *GV = M.getGlobalVariable(Line, true))
        SWMRGlobals.insert(GV);
    }
  }

  Summary.close();
}

AnalysisKey SingleThreaded::Key;

SingleThreaded::Result SingleThreaded::run(Module &M,
                                           ModuleAnalysisManager &AM) {
  if (std::ifstream SummaryFile(SummaryDirName + "/" +
                                SingleThreadedSummaryFileName);
      TsanUseAnalysisSummaries && SummaryFile.good()) {
    LLVM_DEBUG(dbgs() << "Found existing summary file for SingleThreaded "
                         "Analysis. Loading results.\n");
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