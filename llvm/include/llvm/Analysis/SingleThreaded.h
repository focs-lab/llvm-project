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
#include "llvm/Support/CommandLine.h"

namespace llvm {

/// Whether an analysis may seed itself from a summary written while compiling
/// an earlier translation unit.
///
/// Off by default, and deliberately. Reuse is only sound in a build that
/// analyses every module before instrumenting any of them: a global that is
/// consistently lock-protected in the module analysed first may be touched
/// without that lock in the next one, and a summary consulted midway through
/// an ordinary build reports the first answer for the second module. Reading
/// the file also made results depend on the compiler's working directory,
/// which is how one project's summary came to be applied to another's build.
extern cl::opt<bool> TsanUseAnalysisSummaries;

/// Directory summaries are written to and read from, relative to the
/// compiler's working directory.
const std::string SummaryDirName = "tsan-logs";

const std::string SingleThreadedSummaryFileName = "st_summary.txt";
const std::string SummaryHeaderST = "--- Single-Threaded Functions ---";
const std::string SummaryHeaderSWMR =
    "--- Read-Only Global Variables (in Multi-Threaded Context) ---";

// Add names of known thread creation functions here
const SmallDenseSet<StringRef, 8> KnownThreadCreators = {
    "pthread_create", "__pthread_create_2_1", "thrd_create", "__kmpc_fork_call",
    "__kmpc_fork_teams"};

/// Mangled-name prefixes of functions that start a thread. C++, C11 and OpenMP
/// thread creation only reaches pthread_create inside the runtime library, so
/// what is visible in this translation unit is the wrapper, not the creator.
/// Missing these made every function in a std::thread program look
/// single-threaded.
constexpr StringRef ThreadCreatorPrefixes[] = {
    "_ZNSt6thread15_M_start_thread", // libstdc++ std::thread
    "_ZNSt3__16thread6__start",      // libc++ std::thread
};

/// Whether \p F starts a thread, by exact name or mangled prefix.
bool isKnownThreadCreator(const Function &F);

/// Collect the instructions that access \p V, looking through constant
/// expressions.
///
/// A getelementptr on a field or element of a global is a ConstantExpr user,
/// not an Instruction. Enumerating only Instruction users therefore misses
/// every access to an aggregate global -- while the instrumentation pass still
/// resolves those accesses back to the global and acts on whatever was
/// concluded about it.
void collectAccessingInstrs(const Value *V,
                            SmallVectorImpl<const Instruction *> &Out,
                            SmallPtrSetImpl<const Value *> &Visited);

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
  /// context and doesn't create threads.
  ///
  /// Always false for main: main necessarily starts out single-threaded and
  /// then, in most programs, stops being so, and answering at function
  /// granularity would have to pick one of those. Its accesses are classified
  /// individually by the overload below.
  bool isSingleThreaded(const Function *F) const;

  /// Returns true if the given instruction is executed before the program has
  /// created any thread, and so cannot participate in a race.
  bool isSingleThreaded(const Instruction *I) const;

  /// Returns true if the given function is executed in a multithreaded context
  /// or creates threads
  bool isMultithreaded(const Function *F) const { return !isSingleThreaded(F); }

  /// Returns true if the given global variable is only read (not written to) in
  /// multithreaded functions
  bool isSWMRGlobal(const GlobalVariable *GV) const {
    return SWMRGlobals.contains(GV);
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

  SmallPtrSet<const GlobalVariable *, 4> SWMRGlobals;

  // Blocks of main that are already multi-threaded on entry. Blocks outside
  // this set may still turn multi-threaded part-way through, at a thread
  // creating call; isSingleThreaded(const Instruction *) accounts for that.
  SmallPtrSet<const BasicBlock *, 8> MTBlocksInMain;

  /// True if \p BB contains a call that may start a thread.
  bool blockCreatesThreads(const BasicBlock &BB) const;

  /// True if \p I may start a thread.
  bool mayCreateThread(const Instruction &I) const;

  /// Propagate multi-threadedness forward through main's CFG, so that the
  /// prefix of main that runs before any thread exists can still be left
  /// uninstrumented while the rest is not.
  void computeMainMTBlocks();

  /// Mark everything main calls once it is multi-threaded, and everything
  /// those functions call, as multi-threaded too. Without this, a helper
  /// invoked directly from main after a thread has started -- rather than as
  /// the thread body, so its address is never taken -- was classified
  /// single-threaded and left uninstrumented.
  void propagateMTFromMain();

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
  void findSWMRGlobals();

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
