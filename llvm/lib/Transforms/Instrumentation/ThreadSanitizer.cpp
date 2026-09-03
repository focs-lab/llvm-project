//===-- ThreadSanitizer.cpp - race detector -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer, a race detector.
//
// The tool is under development, for the details about previous versions see
// http://code.google.com/p/data-race-test
//
// The instrumentation phase is quite simple:
//   - Insert calls to run-time library before every memory access.
//      - Optimizations may apply to avoid instrumenting some of the accesses.
//   - Insert calls at function entry/exit.
// The rest is handled by the run-time library.
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/ThreadSanitizer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/EscapeAnalysis.h"
#include "llvm/Analysis/LockOwnership.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/CFG.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/SingleThreaded.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/EscapeEnumerator.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopPeel.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/AliasAnalysis.h>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>

using namespace llvm;

#define DEBUG_TYPE "tsan"

static cl::opt<bool> ClInstrumentMemoryAccesses(
    "tsan-instrument-memory-accesses", cl::init(true),
    cl::desc("Instrument memory accesses"), cl::Hidden);
static cl::opt<bool>
    ClInstrumentFuncEntryExit("tsan-instrument-func-entry-exit", cl::init(true),
                              cl::desc("Instrument function entry and exit"),
                              cl::Hidden);
static cl::opt<bool> ClHandleCxxExceptions(
    "tsan-handle-cxx-exceptions", cl::init(true),
    cl::desc("Handle C++ exceptions (insert cleanup blocks for unwinding)"),
    cl::Hidden);
static cl::opt<bool> ClInstrumentAtomics("tsan-instrument-atomics", cl::init(true), cl::desc("Instrument atomics"), cl::Hidden);
static cl::opt<bool> ClInstrumentMemIntrinsics(
    "tsan-instrument-memintrinsics", cl::init(true),
    cl::desc("Instrument memintrinsics (memset/memcpy/memmove)"), cl::Hidden);
static cl::opt<bool> ClDistinguishVolatile(
    "tsan-distinguish-volatile", cl::init(false),
    cl::desc("Emit special instrumentation for accesses to volatiles"),
    cl::Hidden);
static cl::opt<bool> ClInstrumentReadBeforeWrite(
    "tsan-instrument-read-before-write", cl::init(false),
    cl::desc("Do not eliminate read instrumentation for read-before-writes"),
    cl::Hidden);
static cl::opt<bool> ClCompoundReadBeforeWrite(
    "tsan-compound-read-before-write", cl::init(false),
    cl::desc("Emit special compound instrumentation for reads-before-writes"),
    cl::Hidden);
static cl::opt<bool> ClUseEscapeAnalysis(
    "tsan-use-escape-analysis", cl::init(false),
    cl::desc("Use better escape analysis to eliminate extra instrumentation"),
    cl::Hidden);
static cl::opt<bool> ClUseEscapeAnalysisGlobal(
    "tsan-use-escape-analysis-global", cl::init(false),
    cl::desc(
        "Use global (IPA) escape analysis to eliminate extra instrumentation"),
    cl::Hidden);
static cl::opt<bool> ClUseLockOwnershipAnalysis(
    "tsan-use-lock-ownership", cl::init(false),
    cl::desc(
        "Use lock ownership analysis to eliminate extra instrumentation"),
    cl::Hidden);
static cl::opt<bool> ClUseLockOwnershipAnalysisUpperbound(
    "tsan-use-lock-ownership-upperbound", cl::init(false),
    cl::desc("Use lock ownership analysis to eliminate extra instrumentation "
             "-- upper bound estimation (not code inside critical sections "
             "instrumented)"),
    cl::Hidden);
static cl::opt<bool> ClUseSingleThreadedAnalysis(
    "tsan-use-single-threaded", cl::init(false),
    cl::desc("Use single-threaded/multiple-threaded analysis to eliminate "
             "extra instrumentation"),
    cl::Hidden);
static cl::opt<bool> ClUseSWMRAnalysis(
    "tsan-use-swmr", cl::init(false),
    cl::desc("Use single-writer/multiple-reader analysis to eliminate "
             "extra instrumentation"),
    cl::Hidden);
static cl::opt<bool>
    ClUseDominanceAnalysis("tsan-use-dominance-analysis", cl::init(false),
                           cl::desc("Eliminate duplicating instructions which "
                                    "(post)dominates given instruction"),
                           cl::Hidden);
static cl::opt<bool> ClUseDominanceAnalysisDom(
    "tsan-use-dominance-analysis-dom", cl::init(false),
    cl::desc(
        "Eliminate duplicating instructions which dominates given instruction"),
    cl::Hidden);
static cl::opt<bool> ClUseDominanceAnalysisPostDom(
    "tsan-use-dominance-analysis-postdom", cl::init(false),
    cl::desc("Eliminate duplicating instructions which "
             "post-dominates given instruction"),
    cl::Hidden);
static cl::opt<bool> ClPostDomAggressive(
    "tsan-postdom-aggressive", cl::init(false),
    cl::desc("Allow post-dominance elimination across loops (unsafe)"),
    cl::Hidden);
static cl::opt<bool> ClUseLoopPeeling(
    "tsan-use-loop-peeling", cl::init(false),
    cl::desc(
        "Try to peel first iteration to help dominance-based optimization"),
    cl::Hidden);
// Enable/disable the load/store fast-path that skips calling into TSan runtime
// when the runtime reports only one active thread.
static cl::opt<bool> ClTsanUseActiveThreadCountFastPath(
    "tsan-use-active-thread-count", cl::init(false),
    cl::desc("TSan: guard load/store instrumentation with a runtime check of "
             "__tsan_active_thread_count > 1"),
    cl::Hidden);

STATISTIC(NumInstrumentedReads, "Number of instrumented reads");
STATISTIC(NumInstrumentedWrites, "Number of instrumented writes");
STATISTIC(NumOmittedReadsBeforeWrite,
          "Number of reads ignored due to following writes");
STATISTIC(NumAccessesWithBadSize, "Number of accesses with bad size");
STATISTIC(NumInstrumentedVtableWrites, "Number of vtable ptr writes");
STATISTIC(NumInstrumentedVtableReads, "Number of vtable ptr reads");
STATISTIC(NumOmittedReadsFromConstantGlobals,
          "Number of reads from constant globals");
STATISTIC(NumOmittedReadsFromVtable, "Number of vtable reads");
STATISTIC(NumOmittedNonCaptured, "Number of accesses ignored due to capturing");
STATISTIC(NumOmittedNonEscaped,
          "Number of accesses ignored due to non-escaping");

// Statistics for object escape reasons
STATISTIC(NumEscGPTRAliasing, "Number of escapes due to GPTR aliasing");
STATISTIC(NumEscPTRArgAliasing,
          "Number of escapes due to pointer argument aliasing");
STATISTIC(NumEscPassingToCall, "Number of escapes due to passing to a call");
STATISTIC(NumEscRetPtr, "Number of escapes due to returning a pointer");
STATISTIC(NumEscVolatile, "Number of escapes due to volatile");
STATISTIC(NumEscOther, "Number of escapes due to other reasons");
STATISTIC(NumEscInvalid, "Number of escapes due to invalid reasons");
STATISTIC(NumEscCall, "Number of escapes due to escaped calls");
STATISTIC(NumOmittedByDominance, "Number of accesses ignored due to dominance");
STATISTIC(NumOmittedByPostDominance,
          "Number of accesses ignored due to post-dominance");

const char kTsanModuleCtorName[] = "tsan.module_ctor";
const char kTsanInitName[] = "__tsan_init";

namespace {

GlobalVariable *InterceptorEnabled;

/// ThreadSanitizer: instrument the code in module to find races.
///
/// Instantiating ThreadSanitizer inserts the tsan runtime library API function
/// declarations into the module if they don't exist already. Instantiating
/// ensures the __tsan_init function is in the list of global constructors for
/// the module.
struct ThreadSanitizer {
private:
  Module *M = nullptr;
  Function *Func = nullptr;

public:
  ThreadSanitizer() {
    // Check options and warn user.
    if (ClInstrumentReadBeforeWrite && ClCompoundReadBeforeWrite) {
      errs()
          << "warning: Option -tsan-compound-read-before-write has no effect "
             "when -tsan-instrument-read-before-write is set.\n";
    }

    if (ClUseEscapeAnalysisGlobal && ClUseEscapeAnalysis) {
      errs() << "error: Must be chosen only one option from "
                "-tsan-escape-analysis or -tsan-escape-analysis-global\n";
      llvm_shutdown();
    }
  }

//#define INSTR_STAT_ENABLED 1
#ifdef INSTR_STAT_ENABLED
  ~ThreadSanitizer() {
    std::string FullPath;
    const unsigned PathLength = 2048;
    if (M) {
      // Most reliable way: use debug information
      auto *CompileUnits = M->getNamedMetadata("llvm.dbg.cu");
      if (CompileUnits)
        for (const auto *Node : CompileUnits->operands())
          if (const auto *CU = llvm::dyn_cast<llvm::DICompileUnit>(Node)) {
            DIFile *File = CU->getFile();
            if (File) {
              SmallString<PathLength> path(File->getDirectory());
              sys::path::append(path, File->getFilename());
              FullPath = std::string(path.str());
              break; // Take path from first found compilation unit
            }
          }

      // Fallback option if no debug information is available
      if (FullPath.empty())
        FullPath = M->getSourceFileName();
    }

    // --- Step 2: Create directory /tmp/__tsan__ ---
    const char *DirName = "/tmp/__tsan__";
    std::error_code EC = sys::fs::create_directory(DirName);
    if (EC) {
      errs() << "TSan: Failed to create directory " << DirName << ": "
             << EC.message() << "\n";
      return;
    }

    // --- Step 3: Create filename and write result ---
    // Convert path to safe filename by replacing separators
    std::string OutputFileName = FullPath;
    std::replace(OutputFileName.begin(), OutputFileName.end(), '/', '_');
    std::replace(OutputFileName.begin(), OutputFileName.end(), '\\', '_');

    SmallString<PathLength> FinalFilePath(DirName);
    sys::path::append(FinalFilePath, OutputFileName);

    const auto NumInstrumentedInstructions =
        NumInstrumentedReads + NumInstrumentedWrites;

    // --- Step 3: Read file and check condition ---
    bool ShouldWrite = true;
    if (sys::fs::exists(FinalFilePath)) {
      // Try to read file contents
      auto FileOrErr = MemoryBuffer::getFile(FinalFilePath);
      if (std::error_code ec = FileOrErr.getError()) {
        errs() << "TSan: Failed to read existing file " << FinalFilePath << ": "
               << ec.message() << "\n";
        ShouldWrite = false; // Can't read, so don't overwrite
      } else {
        StringRef Content = (*FileOrErr)->getBuffer().trim();
        if (!Content.empty()) {
          unsigned existingValue;
          if (Content.getAsInteger(10, existingValue)) {
            // If failed to convert string to number, consider it an error
            errs() << "TSan: Failed to parse number in file " << FinalFilePath
                   << ". Content: \"" << Content << "\"\n";
            ShouldWrite = false;
          } else {
            // Check condition
            if (existingValue > NumInstrumentedInstructions) {
              errs() << "TSan: Check failed! Existing value (" << existingValue
                     << ") is greater than new value ("
                     << NumInstrumentedInstructions << ") for file "
                     << FinalFilePath << "\n";
              ShouldWrite = false;
            }
          }
        }
      }
    }

    // --- Step 4: Write statistics
    if (ShouldWrite) {
      std::error_code FileEC;
      raw_fd_ostream OutStream(FinalFilePath, FileEC, llvm::sys::fs::OF_None);
      if (FileEC) {
        errs() << "TSan: Error opening file " << FinalFilePath << ": "
               << FileEC.message() << "\n";
        return;
      }

      // Write the sum of instrumented instructions
      OutStream << NumInstrumentedInstructions << "\n";
      // errs() << "TSan: Wrote " << NumInstrumentedInstructions
      //        << " instructions to file " << FinalFilePath << "\n";
    }
  }
#endif

  bool sanitizeFunction(
      Function &F, const TargetLibraryInfo &TLI,
      const std::optional<EscapeAnalysisInfo> &EAI,
      std::optional<EscapeAnalysisGlobalInfo *> EAIGlobal = std::nullopt,
      std::optional<LockOwnershipInfo *> LOI = std::nullopt,
      std::optional<SingleThreadedInfo *> STI = std::nullopt,
      DominatorTree *DT = nullptr, PostDominatorTree *PDT = nullptr,
      AAResults *AA = nullptr, LoopInfo *LI = nullptr,
      AssumptionCache *AC = nullptr, ScalarEvolution *SE = nullptr);

  /// Checks if an instruction could potentially change ThreadSanitizer's
  /// synchronization state. This includes atomic operations, memory barriers,
  /// certain intrinsics and external calls.
  /// @param Inst The instruction to check
  /// @param TLI Target library info to identify standard library functions
  /// @return true if instruction could affect synchronization state, false if
  /// proven safe
  /// Synchronization effects an instruction may have, as a bitmask. The two
  /// lock bits are directional, and that direction is what lets us cross a
  /// lock at all:
  ///
  ///   - Dominance removes the *later* access and relies on the earlier one.
  ///     An acquire between them can only order some remote event *before* the
  ///     later access, which cannot create a race the earlier one misses; a
  ///     release can, so it blocks.
  ///   - Post-dominance removes the *earlier* access and relies on the later
  ///     one, so the two roles are exactly reversed.
  ///
  /// Termination is a separate axis: post-dominance additionally needs the
  /// covering access to actually be reached.
  enum SyncEffect : unsigned {
    SYNC_NONE = 0,
    SYNC_ACQUIRE = 1u << 0,
    SYNC_RELEASE = 1u << 1,
    SYNC_UNKNOWN = 1u << 2,
    SYNC_MAY_NOT_RETURN = 1u << 3,

    /// Bits that make a path unusable for dominance elimination ...
    SYNC_BLOCKS_DOM = SYNC_RELEASE | SYNC_UNKNOWN,
    /// ... and for post-dominance elimination.
    SYNC_BLOCKS_POSTDOM = SYNC_ACQUIRE | SYNC_UNKNOWN | SYNC_MAY_NOT_RETURN,
  };

  /// Classify what \p Inst may do to ThreadSanitizer's happens-before state.
  static unsigned classifySyncEffect(const Instruction *Inst,
                                     const TargetLibraryInfo &TLI);

  /// True if \p Inst may affect synchronization state at all. Used by the
  /// interprocedural sync-free analysis, which has no notion of direction.
  static bool isInstrDangerous(const Instruction *Inst,
                               const TargetLibraryInfo &TLI);

private:
  // Internal Instruction wrapper that contains more information about the
  // Instruction from prior analysis.
  struct InstructionInfo {
    // Instrumentation emitted for this instruction is for a compounded set of
    // read and write operations in the same basic block.
    static constexpr unsigned kCompoundRW = (1U << 0);

    explicit InstructionInfo(Instruction *Inst) : Inst(Inst) {}

    /// A compound read-modify-write is instrumented as a write, and a write
    /// races with both remote reads and remote writes.
    bool isWriteOperation() const {
      return isa<StoreInst>(Inst) || (Flags & kCompoundRW);
    }

    Instruction *Inst;
    unsigned Flags = 0;
  };

  void initialize(Module &M, const TargetLibraryInfo &TLI);
  Value *checkActiveThreadCount(IRBuilderBase &IRB, Module &M);
  bool instrumentLoadOrStore(const InstructionInfo &II, const DataLayout &DL);
  bool instrumentAtomic(Instruction *I, const DataLayout &DL);
  void disableInterceptorForInstr(Instruction *I, InstrumentationIRBuilder &IRB);
  bool instrumentInterceptedCalls(
      CallInst *CI, const TargetLibraryInfo &TLI,
      std::optional<EscapeAnalysisGlobalInfo *> EAIGlobal);
  bool
  instrumentMemIntrinsic(Instruction *I, const TargetLibraryInfo &TLI,
                         std::optional<EscapeAnalysisGlobalInfo *> EAIGlobal);
  void chooseInstructionsToInstrument(
      SmallVectorImpl<Instruction *> &Local,
      SmallVectorImpl<InstructionInfo> &All, const TargetLibraryInfo &TLI,
      const DataLayout &DL, const std::optional<EscapeAnalysisInfo> &EAI,
      std::optional<EscapeAnalysisGlobalInfo *> EAIGlobal = std::nullopt,
      std::optional<LockOwnershipInfo *> LOI = std::nullopt,
      std::optional<SingleThreadedInfo *> STI = std::nullopt);

  DenseMap<Instruction *, size_t> createInstrIndexMap(
      SmallVectorImpl<InstructionInfo> &AllInstr);
  template <bool IsPostDom>
  void eliminateInstrByPrePostDominance(
      SmallVectorImpl<InstructionInfo> &AllInstr,
      const DominatorTreeBase<BasicBlock, IsPostDom> *DTBase, AAResults *AA,
      const TargetLibraryInfo &TLI, const LoopInfo *LI, ScalarEvolution *SE);

  /// True if DomInst's instrumentation call covers everything CurrInst's
  /// would: the same address, and at least as many bytes.
  static bool locationCovers(Instruction *DomInst, Instruction *CurrInst,
                             AAResults *AA);

  /// Union of the synchronization effects over every path from \p FirstInst to
  /// \p SecondInst in program order, plus the cycle through \p RemovedInst
  /// when it lies on one.
  unsigned scanPaths(Instruction *FirstInst, Instruction *SecondInst,
                     Instruction *RemovedInst, const TargetLibraryInfo &TLI,
                     const LoopInfo *LI, ScalarEvolution *SE,
                     bool NeedTermination);

  /// Blocks from which \p BB is reachable, memoized for the current function.
  const SmallPtrSetImpl<const BasicBlock *> *
  getReverseReachable(const BasicBlock *BB);

  DenseMap<const BasicBlock *, SmallPtrSet<const BasicBlock *, 32>>
      RevReachCache;

  bool addrPointsToConstantData(Value *Addr);
  int getMemoryAccessFuncIndex(Type *OrigTy, Value *Addr, const DataLayout &DL);
  void InsertRuntimeIgnores(Function &F);

  Type *IntptrTy;
  FunctionCallee TsanFuncEntry;
  FunctionCallee TsanFuncExit;
  FunctionCallee TsanIgnoreBegin;
  FunctionCallee TsanIgnoreEnd;
  // Accesses sizes are powers of two: 1, 2, 4, 8, 16.
  static const size_t kNumberOfAccessSizes = 5;
  FunctionCallee TsanRead[kNumberOfAccessSizes];
  FunctionCallee TsanWrite[kNumberOfAccessSizes];
  FunctionCallee TsanUnalignedRead[kNumberOfAccessSizes];
  FunctionCallee TsanUnalignedWrite[kNumberOfAccessSizes];
  FunctionCallee TsanVolatileRead[kNumberOfAccessSizes];
  FunctionCallee TsanVolatileWrite[kNumberOfAccessSizes];
  FunctionCallee TsanUnalignedVolatileRead[kNumberOfAccessSizes];
  FunctionCallee TsanUnalignedVolatileWrite[kNumberOfAccessSizes];
  FunctionCallee TsanCompoundRW[kNumberOfAccessSizes];
  FunctionCallee TsanUnalignedCompoundRW[kNumberOfAccessSizes];
  FunctionCallee TsanAtomicLoad[kNumberOfAccessSizes];
  FunctionCallee TsanAtomicStore[kNumberOfAccessSizes];
  FunctionCallee TsanAtomicRMW[AtomicRMWInst::LAST_BINOP + 1]
                              [kNumberOfAccessSizes];
  FunctionCallee TsanAtomicCAS[kNumberOfAccessSizes];
  FunctionCallee TsanAtomicThreadFence;
  FunctionCallee TsanAtomicSignalFence;
  FunctionCallee TsanVptrUpdate;
  FunctionCallee TsanVptrLoad;
  FunctionCallee MemmoveFn, MemcpyFn, MemsetFn;

  // Instrinsics for disabling/enabling instrumentation
  // for specific code section
  FunctionCallee TsanDisableFn, TsanEnableFn;
};

void insertModuleCtor(Module &M) {
  getOrCreateSanitizerCtorAndInitFunctions(
      M, kTsanModuleCtorName, kTsanInitName, /*InitArgTypes=*/{},
      /*InitArgs=*/{},
      // This callback is invoked when the functions are created the first
      // time. Hook them into the global ctors list in that case:
      [&](Function *Ctor, FunctionCallee) { appendToGlobalCtors(M, Ctor, 0); });
}
}  // namespace

static bool tryPeelLoops(Function &F, LoopInfo *LI, ScalarEvolution *SE,
                         DominatorTree *DT, AssumptionCache *AC) {
  if (!LI || !SE || !DT || F.isDeclaration())
    return false;
  LLVM_DEBUG(dbgs() << "Trying to peel loops in function " << F.getName()
                    << "\n");
  bool Changed = false;

  // Collect all innermost loops
  SmallVector<Loop *, 8> Worklist;
  for (Loop *L : LI->getLoopsInPreorder()) {
    if (L->isInnermost())
      Worklist.push_back(L);
  }

  for (Loop *L : Worklist) {
    // Put the loop into LoopSimplify form if needed.
    if (!L->isLoopSimplifyForm()) {
      // `simplifyLoop` returns true if it changed the CFG.
      // Pass DT/LI/SE/AC so the analyses stay up to date.
      if (!simplifyLoop(L, DT, LI, SE, AC, nullptr, false)) {
        LLVM_DEBUG(dbgs() << "Skipping peeling: cannot simplify loop " << *L
                          << "\n");
        continue;
      }
      Changed = true;
    }

    // Now that the loop is simplified, check if it can be peeled.
    if (!canPeel(L)) {
      LLVM_DEBUG(dbgs() << "Cannot peel loop " << *L << "\n");
      continue;
    }

    if (!L->isRecursivelyLCSSAForm(*DT, *LI))
      formLCSSARecursively(*L, *DT, LI, SE);

    // Peel one iteration.
    ValueToValueMapTy LVMap;
    if (peelLoop(L, /*PeelCount=*/1, LI, SE, *DT, AC, false, LVMap)) {
      LLVM_DEBUG(dbgs() << "Loop peeled: " << *L);
      Changed = true;
    }
  }
  return Changed;
}

PreservedAnalyses ThreadSanitizerPass::run(Function &F,
                                           FunctionAnalysisManager &FAM) {
  ThreadSanitizer TSan;

  if (ClUseEscapeAnalysis) {
    if (TSan.sanitizeFunction(F, FAM.getResult<TargetLibraryAnalysis>(F),
                              FAM.getResult<EscapeAnalysis>(F), std::nullopt,
                              std::nullopt, std::nullopt, nullptr, nullptr,
                              nullptr))
      return PreservedAnalyses::none();
  }

  std::optional<EscapeAnalysisGlobalInfo *> EAGI = std::nullopt;
  std::optional<LockOwnershipInfo *> LOI = std::nullopt;
  std::optional<SingleThreadedInfo *> STI = std::nullopt;

  const auto &MAMProxy = FAM.getResult<ModuleAnalysisManagerFunctionProxy>(F);

  if (ClUseEscapeAnalysisGlobal)
    EAGI = MAMProxy.getCachedResult<EscapeAnalysisGlobal>(*F.getParent());
  if (ClUseSingleThreadedAnalysis || ClUseSWMRAnalysis)
    STI = MAMProxy.getCachedResult<SingleThreaded>(*F.getParent());
  if (ClUseLockOwnershipAnalysis || ClUseLockOwnershipAnalysisUpperbound)
    LOI = MAMProxy.getCachedResult<LockOwnership>(*F.getParent());

  DominatorTree *DT = nullptr;
  PostDominatorTree *PDT = nullptr;
  AAResults *AA = nullptr;
  LoopInfo *LI = nullptr;
  AssumptionCache *AC = nullptr;
  ScalarEvolution *SE = nullptr;
  if (ClUseDominanceAnalysis || ClUseDominanceAnalysisDom ||
      ClUseDominanceAnalysisPostDom) {
    DT = &FAM.getResult<DominatorTreeAnalysis>(F);
    PDT = &FAM.getResult<PostDominatorTreeAnalysis>(F);
    AA = &FAM.getResult<AAManager>(F);
    LI = &FAM.getResult<LoopAnalysis>(F);
    AC = &FAM.getResult<AssumptionAnalysis>(F);
    // Post-dominance needs to know whether a loop between two accesses can
    // spin forever; a computable backedge-taken count settles it.
    SE = &FAM.getResult<ScalarEvolutionAnalysis>(F);
  }

  const bool Instrumented =
      TSan.sanitizeFunction(F, FAM.getResult<TargetLibraryAnalysis>(F),
                            std::nullopt, EAGI, LOI, STI, DT, PDT, AA, LI, AC,
                            SE);
  if (Instrumented)
    return PreservedAnalyses::none();

  return PreservedAnalyses::all();
}

std::unique_ptr<SyncFreeInfo> ModuleThreadSanitizerPass::SFI;

PreservedAnalyses ModuleThreadSanitizerPass::run(Module &M,
                                                 ModuleAnalysisManager &MAM) {
  ////
  // First try to peel loops (before any analysis)
  // Try to peel loops to allow domination-based optimization
  if (ClUseLoopPeeling &&
      (ClUseDominanceAnalysis || ClUseDominanceAnalysisDom)) {
    FunctionAnalysisManager &FAM =
        MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    DominatorTree *DT = nullptr;
    LoopInfo *LI = nullptr;
    AssumptionCache *AC = nullptr;
    ScalarEvolution *SE = nullptr;
    bool ModuleCodeChange = false;

    for (auto &F: M) {
      if (F.isDeclaration())
        continue;
      DT = &FAM.getResult<DominatorTreeAnalysis>(F);
      LI = &FAM.getResult<LoopAnalysis>(F);
      SE = &FAM.getResult<ScalarEvolutionAnalysis>(F);
      AC = &FAM.getResult<AssumptionAnalysis>(F);

      bool CodeChange = tryPeelLoops(F, LI, SE, DT, AC);
      if (CodeChange)
        FAM.invalidate(F, PreservedAnalyses::none());
      ModuleCodeChange |= CodeChange;
    }
    if (ModuleCodeChange)
      MAM.invalidate(M, PreservedAnalyses::none());
  }
  ////

  LLVM_DEBUG({
    dbgs() << "-- Module " << M.getName() << ": ";
    if (ClUseEscapeAnalysis)
      dbgs() << "escape analysis; ";
    else if (ClUseEscapeAnalysisGlobal)
      dbgs() << "global escape analysis; ";
    else
      dbgs() << "capture tracker; ";
    if (ClUseLockOwnershipAnalysis)
      dbgs() << "lock ownership; ";
    if (ClUseSingleThreadedAnalysis)
      dbgs() << "single/multi-threaded; ";
    if (ClUseSWMRAnalysis)
      dbgs() << "SWMR; ";
    if (ClUseDominanceAnalysis || ClUseDominanceAnalysisDom ||
        ClUseDominanceAnalysisPostDom)
      dbgs() << "dominance; ";
    dbgs() << "--\n";
  });

  if (ClUseDominanceAnalysis || ClUseDominanceAnalysisDom ||
      ClUseDominanceAnalysisPostDom) {
    SFI = std::make_unique<SyncFreeInfo>(
        M, MAM.getResult<CallGraphAnalysis>(M),
        MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager());
  }

  if (ClUseLockOwnershipAnalysis && ClUseLockOwnershipAnalysisUpperbound)
    errs() << "warning: -tsan-use-lock-ownership and "
              "-tsan-use-lock-ownership-upperbound are mutually exclusive\n";

  if (ClUseEscapeAnalysisGlobal)
    MAM.getResult<EscapeAnalysisGlobal>(M);

  if (ClUseLockOwnershipAnalysis || ClUseLockOwnershipAnalysisUpperbound)
    MAM.getResult<LockOwnership>(M);

  if (ClUseSingleThreadedAnalysis || ClUseSWMRAnalysis)
    MAM.getResult<SingleThreaded>(M);

  insertModuleCtor(M);

  // Declare an external global variable InterceptorEnabled in the module
  // Thread-local: the runtime defines it as THREADLOCAL, and a process-wide
  // flag would let one thread's window suppress interceptors in all others.
  InterceptorEnabled = new GlobalVariable(
      M, Type::getInt1Ty(M.getContext()), /*isConstant=*/false,
      GlobalValue::ExternalLinkage, nullptr, "InterceptorEnabled", nullptr,
      GlobalValue::GeneralDynamicTLSModel);

  return PreservedAnalyses::none();
}

void ThreadSanitizer::initialize(Module &M, const TargetLibraryInfo &TLI) {
  const DataLayout &DL = M.getDataLayout();
  LLVMContext &Ctx = M.getContext();
  IntptrTy = DL.getIntPtrType(Ctx);

  IRBuilder<> IRB(Ctx);
  AttributeList Attr;
  Attr = Attr.addFnAttribute(Ctx, Attribute::NoUnwind);
  // Initialize the callbacks.
  TsanFuncEntry = M.getOrInsertFunction("__tsan_func_entry", Attr,
                                        IRB.getVoidTy(), IRB.getPtrTy());
  TsanFuncExit =
      M.getOrInsertFunction("__tsan_func_exit", Attr, IRB.getVoidTy());
  TsanIgnoreBegin = M.getOrInsertFunction("__tsan_ignore_thread_begin", Attr,
                                          IRB.getVoidTy());
  TsanIgnoreEnd =
      M.getOrInsertFunction("__tsan_ignore_thread_end", Attr, IRB.getVoidTy());
  IntegerType *OrdTy = IRB.getInt32Ty();
  for (size_t i = 0; i < kNumberOfAccessSizes; ++i) {
    const unsigned ByteSize = 1U << i;
    const unsigned BitSize = ByteSize * 8;
    std::string ByteSizeStr = utostr(ByteSize);
    std::string BitSizeStr = utostr(BitSize);
    SmallString<32> ReadName("__tsan_read" + ByteSizeStr);
    TsanRead[i] = M.getOrInsertFunction(ReadName, Attr, IRB.getVoidTy(),
                                        IRB.getPtrTy());

    SmallString<32> WriteName("__tsan_write" + ByteSizeStr);
    TsanWrite[i] = M.getOrInsertFunction(WriteName, Attr, IRB.getVoidTy(),
                                         IRB.getPtrTy());

    SmallString<64> UnalignedReadName("__tsan_unaligned_read" + ByteSizeStr);
    TsanUnalignedRead[i] = M.getOrInsertFunction(
        UnalignedReadName, Attr, IRB.getVoidTy(), IRB.getPtrTy());

    SmallString<64> UnalignedWriteName("__tsan_unaligned_write" + ByteSizeStr);
    TsanUnalignedWrite[i] = M.getOrInsertFunction(
        UnalignedWriteName, Attr, IRB.getVoidTy(), IRB.getPtrTy());

    SmallString<64> VolatileReadName("__tsan_volatile_read" + ByteSizeStr);
    TsanVolatileRead[i] = M.getOrInsertFunction(
        VolatileReadName, Attr, IRB.getVoidTy(), IRB.getPtrTy());

    SmallString<64> VolatileWriteName("__tsan_volatile_write" + ByteSizeStr);
    TsanVolatileWrite[i] = M.getOrInsertFunction(
        VolatileWriteName, Attr, IRB.getVoidTy(), IRB.getPtrTy());

    SmallString<64> UnalignedVolatileReadName("__tsan_unaligned_volatile_read" +
                                              ByteSizeStr);
    TsanUnalignedVolatileRead[i] = M.getOrInsertFunction(
        UnalignedVolatileReadName, Attr, IRB.getVoidTy(), IRB.getPtrTy());

    SmallString<64> UnalignedVolatileWriteName(
        "__tsan_unaligned_volatile_write" + ByteSizeStr);
    TsanUnalignedVolatileWrite[i] = M.getOrInsertFunction(
        UnalignedVolatileWriteName, Attr, IRB.getVoidTy(), IRB.getPtrTy());

    SmallString<64> CompoundRWName("__tsan_read_write" + ByteSizeStr);
    TsanCompoundRW[i] = M.getOrInsertFunction(
        CompoundRWName, Attr, IRB.getVoidTy(), IRB.getPtrTy());

    SmallString<64> UnalignedCompoundRWName("__tsan_unaligned_read_write" +
                                            ByteSizeStr);
    TsanUnalignedCompoundRW[i] = M.getOrInsertFunction(
        UnalignedCompoundRWName, Attr, IRB.getVoidTy(), IRB.getPtrTy());

    Type *Ty = Type::getIntNTy(Ctx, BitSize);
    Type *PtrTy = PointerType::get(Ctx, 0);
    SmallString<32> AtomicLoadName("__tsan_atomic" + BitSizeStr + "_load");
    TsanAtomicLoad[i] =
        M.getOrInsertFunction(AtomicLoadName,
                              TLI.getAttrList(&Ctx, {1}, /*Signed=*/true,
                                              /*Ret=*/BitSize <= 32, Attr),
                              Ty, PtrTy, OrdTy);

    // Args of type Ty need extension only when BitSize is 32 or less.
    using Idxs = std::vector<unsigned>;
    Idxs Idxs2Or12   ((BitSize <= 32) ? Idxs({1, 2})       : Idxs({2}));
    Idxs Idxs34Or1234((BitSize <= 32) ? Idxs({1, 2, 3, 4}) : Idxs({3, 4}));
    SmallString<32> AtomicStoreName("__tsan_atomic" + BitSizeStr + "_store");
    TsanAtomicStore[i] = M.getOrInsertFunction(
        AtomicStoreName,
        TLI.getAttrList(&Ctx, Idxs2Or12, /*Signed=*/true, /*Ret=*/false, Attr),
        IRB.getVoidTy(), PtrTy, Ty, OrdTy);

    for (unsigned Op = AtomicRMWInst::FIRST_BINOP;
         Op <= AtomicRMWInst::LAST_BINOP; ++Op) {
      TsanAtomicRMW[Op][i] = nullptr;
      const char *NamePart = nullptr;
      if (Op == AtomicRMWInst::Xchg)
        NamePart = "_exchange";
      else if (Op == AtomicRMWInst::Add)
        NamePart = "_fetch_add";
      else if (Op == AtomicRMWInst::Sub)
        NamePart = "_fetch_sub";
      else if (Op == AtomicRMWInst::And)
        NamePart = "_fetch_and";
      else if (Op == AtomicRMWInst::Or)
        NamePart = "_fetch_or";
      else if (Op == AtomicRMWInst::Xor)
        NamePart = "_fetch_xor";
      else if (Op == AtomicRMWInst::Nand)
        NamePart = "_fetch_nand";
      else
        continue;
      SmallString<32> RMWName("__tsan_atomic" + itostr(BitSize) + NamePart);
      TsanAtomicRMW[Op][i] = M.getOrInsertFunction(
          RMWName,
          TLI.getAttrList(&Ctx, Idxs2Or12, /*Signed=*/true,
                          /*Ret=*/BitSize <= 32, Attr),
          Ty, PtrTy, Ty, OrdTy);
    }

    SmallString<32> AtomicCASName("__tsan_atomic" + BitSizeStr +
                                  "_compare_exchange_val");
    TsanAtomicCAS[i] = M.getOrInsertFunction(
        AtomicCASName,
        TLI.getAttrList(&Ctx, Idxs34Or1234, /*Signed=*/true,
                        /*Ret=*/BitSize <= 32, Attr),
        Ty, PtrTy, Ty, Ty, OrdTy, OrdTy);
  }
  TsanVptrUpdate =
      M.getOrInsertFunction("__tsan_vptr_update", Attr, IRB.getVoidTy(),
                            IRB.getPtrTy(), IRB.getPtrTy());
  TsanVptrLoad = M.getOrInsertFunction("__tsan_vptr_read", Attr,
                                       IRB.getVoidTy(), IRB.getPtrTy());
  TsanAtomicThreadFence = M.getOrInsertFunction(
      "__tsan_atomic_thread_fence",
      TLI.getAttrList(&Ctx, {0}, /*Signed=*/true, /*Ret=*/false, Attr),
      IRB.getVoidTy(), OrdTy);

  TsanAtomicSignalFence = M.getOrInsertFunction(
      "__tsan_atomic_signal_fence",
      TLI.getAttrList(&Ctx, {0}, /*Signed=*/true, /*Ret=*/false, Attr),
      IRB.getVoidTy(), OrdTy);

  MemmoveFn =
      M.getOrInsertFunction("__tsan_memmove", Attr, IRB.getPtrTy(),
                            IRB.getPtrTy(), IRB.getPtrTy(), IntptrTy);
  MemcpyFn =
      M.getOrInsertFunction("__tsan_memcpy", Attr, IRB.getPtrTy(),
                            IRB.getPtrTy(), IRB.getPtrTy(), IntptrTy);
  MemsetFn = M.getOrInsertFunction(
      "__tsan_memset",
      TLI.getAttrList(&Ctx, {1}, /*Signed=*/true, /*Ret=*/false, Attr),
      IRB.getPtrTy(), IRB.getPtrTy(), IRB.getInt32Ty(), IntptrTy);

  /////////////////////////////////////////////////////////////////////////////
  // This code is for disabling/enabling instrumentation
  // for specific code section
  TsanEnableFn = M.getOrInsertFunction("__tsan_enable", Attr, IRB.getVoidTy());
  TsanDisableFn = M.getOrInsertFunction("__tsan_disable", Attr, IRB.getVoidTy());
}

static bool isVtableAccess(Instruction *I) {
  if (MDNode *Tag = I->getMetadata(LLVMContext::MD_tbaa))
    return Tag->isTBAAVtableAccess();
  return false;
}

// Do not instrument known races/"benign races" that come from compiler
// instrumentatin. The user has no way of suppressing them.
static bool shouldInstrumentReadWriteFromAddress(const Module *M, Value *Addr) {
  // Peel off GEPs and BitCasts.
  Addr = Addr->stripInBoundsOffsets();

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    if (GV->hasSection()) {
      StringRef SectionName = GV->getSection();
      // Check if the global is in the PGO counters section.
      auto OF = Triple(M->getTargetTriple()).getObjectFormat();
      if (SectionName.ends_with(
              getInstrProfSectionName(IPSK_cnts, OF, /*AddSegmentInfo=*/false)))
        return false;
    }
  }

  // Do not instrument accesses from different address spaces; we cannot deal
  // with them.
  if (Addr) {
    Type *PtrTy = cast<PointerType>(Addr->getType()->getScalarType());
    if (PtrTy->getPointerAddressSpace() != 0)
      return false;
  }

  return true;
}

bool ThreadSanitizer::addrPointsToConstantData(Value *Addr) {
  // If this is a GEP, just analyze its pointer operand.
  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Addr))
    Addr = GEP->getPointerOperand();

  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(Addr)) {
    if (GV->isConstant()) {
      // Reads from constant globals can not race with any writes.
      NumOmittedReadsFromConstantGlobals++;
      return true;
    }
  } else if (LoadInst *L = dyn_cast<LoadInst>(Addr)) {
    if (isVtableAccess(L)) {
      // Reads from a vtable pointer can not race with any writes.
      NumOmittedReadsFromVtable++;
      return true;
    }
  }
  return false;
}

using EscReasonTy = EscapeAnalysisInfo::EscReasonTy;

static void updateEscapeStatistics(EscReasonTy Reason) {
  if ((Reason & EscReasonTy(EscapeAnalysisInfo::GPTR_ALIASING)).any())
    ++NumEscGPTRAliasing;
  if ((Reason & EscReasonTy(EscapeAnalysisInfo::PTR_ARG_ALIASING)).any())
    ++NumEscPTRArgAliasing;
  if ((Reason & EscReasonTy(EscapeAnalysisInfo::PASSING_TO_CALL)).any())
    ++NumEscPassingToCall;
  if ((Reason & EscReasonTy(EscapeAnalysisInfo::RET_PTR)).any())
    ++NumEscRetPtr;
  if ((Reason & EscReasonTy(EscapeAnalysisInfo::VOLATILE)).any())
    ++NumEscVolatile;
  if ((Reason & EscReasonTy(EscapeAnalysisInfo::OTHER)).any())
    ++NumEscOther;
  if ((Reason & EscReasonTy(EscapeAnalysisInfo::INVALID)).any())
    ++NumEscInvalid;
  if ((Reason & EscReasonTy(EscapeAnalysisInfo::ESCAPED_CALL)).any())
    ++NumEscCall;
}

// Instrumenting some of the accesses may be proven redundant.
// Currently handled:
//  - read-before-write (within same BB, no calls between)
//  - not captured variables
//
// We do not handle some of the patterns that should not survive
// after the classic compiler optimizations.
// E.g. two reads from the same temp should be eliminated by CSE,
// two writes should be eliminated by DSE, etc.
//
// 'Local' is a vector of insns within the same BB (no calls between).
// 'All' is a vector of insns that will be instrumented.
void ThreadSanitizer::chooseInstructionsToInstrument(
    SmallVectorImpl<Instruction *> &Local,
    SmallVectorImpl<InstructionInfo> &All, const TargetLibraryInfo &TLI,
    const DataLayout &DL, const std::optional<EscapeAnalysisInfo> &EAI,
    std::optional<EscapeAnalysisGlobalInfo *> EAIGlobal,
    std::optional<LockOwnershipInfo *> LOI,
    std::optional<SingleThreadedInfo *> STI) {
  DenseMap<Value *, size_t> WriteTargets; // Map of addresses to index in All
  // Iterate from the end.
  for (Instruction *I : reverse(Local)) {
    LLVM_DEBUG(dbgs() << "\nchooseI: " << *I << "\n");

    const bool IsWrite = isa<StoreInst>(*I);
    Value *Addr = IsWrite ? cast<StoreInst>(I)->getPointerOperand()
                          : cast<LoadInst>(I)->getPointerOperand();

    if (!shouldInstrumentReadWriteFromAddress(I->getModule(), Addr))
      continue;

    if (!IsWrite) {
      const auto WriteEntry = WriteTargets.find(Addr);
      if (!ClInstrumentReadBeforeWrite && WriteEntry != WriteTargets.end()) {
        auto &WI = All[WriteEntry->second];
        // If we distinguish volatile accesses and if either the read or write
        // is volatile, do not omit any instrumentation.
        const bool AnyVolatile =
            ClDistinguishVolatile && (cast<LoadInst>(I)->isVolatile() ||
                                      cast<StoreInst>(WI.Inst)->isVolatile());
        if (!AnyVolatile) {
          // We will write to this temp, so no reason to analyze the read.
          // Mark the write instruction as compound.
          WI.Flags |= InstructionInfo::kCompoundRW;
          NumOmittedReadsBeforeWrite++;
          continue;
        }
      }

      if (addrPointsToConstantData(Addr)) {
        // Addr points to some constant data -- it can not race with any writes.
        continue;
      }
    }

    // 1. Default (capture tracking)
    if (isa<AllocaInst>(getUnderlyingObject(Addr))) {
      if (!PointerMayBeCaptured(Addr, true, true)) {
        LLVM_DEBUG(dbgs() << "PointerMayBeCaptured -- Instruction omitted\n");
        NumOmittedNonCaptured++;
        continue;
      }
    }

    // 2. If escape analysis is enabled
    if (EAI.has_value()) {
      bool InstrOmitted = false;
      for (const UnderlObjTy &UnderlObj :
           EscapeAnalysisInfo::getUnderlyingMayEscObjs(Addr, TLI)) {
        EscReasonTy EscReason;
        const bool IsEscaped = EAI.value().isEscapedForBB(
            I->getParent(), UnderlObj, &EscReason);
        if (IsEscaped) {
          InstrOmitted = false;
          break;
        }
        InstrOmitted = true;
      }
      if (InstrOmitted) {
        LLVM_DEBUG(dbgs() << "Instruction omitted\n");
        NumOmittedNonEscaped++;
        continue;
      }
    } else if (EAIGlobal.has_value()) {
      EscReasonTy EscReason;
      if (!EAIGlobal.value()->isEscapedUndrlObjOrPointee(
              Addr, TLI, I->getParent(), EscReason)) {
        LLVM_DEBUG(dbgs() << "Instruction omitted due to escape analysis\n");
        NumOmittedNonEscaped++;
        continue;
      }
      updateEscapeStatistics(EscReason);
    }

    // 3. If lock ownership analysis is available
    if (LOI.has_value()) {
      if (ClUseLockOwnershipAnalysisUpperbound) {
        LLVM_DEBUG(dbgs() << "Lock ownership analysis -- upper bound\n");
        if (LOI.value()->isInsideCriticalSection(I)) {
          if (const Value *V = getUnderlyingObject(Addr);
              isa<GlobalVariable>(V)) {
            LLVM_DEBUG(dbgs() << "Instruction omitted due to lock ownership\n");
            continue;
          }
        }
      } else if (ClUseLockOwnershipAnalysis) {
        LLVM_DEBUG(dbgs() << "Lock ownership analysis\n");
        if (const Value *V = getUnderlyingObject(Addr))
          if (const auto *GV = dyn_cast<GlobalVariable>(V))
            if (LOI.value()->isProtectedGV(GV)) {
              LLVM_DEBUG(dbgs()
                         << "Instruction omitted due to lock ownership\n");
              continue;
            }
      }
    }

    // 3b. Skip accesses that run before the program has created any thread.
    // Whole functions are skipped earlier, in sanitizeFunction; this catches
    // main, whose prefix is single-threaded and whose remainder usually is
    // not, so it can only be classified one access at a time.
    if (ClUseSingleThreadedAnalysis && STI.has_value() &&
        STI.value()->isSingleThreaded(I)) {
      LLVM_DEBUG(dbgs() << "Instruction omitted: single-threaded context\n");
      continue;
    }

    // 4. Skip instrumentation if SWMR (Single-Writer/Multiple-Reader) analysis is
    // enabled and indicates this global variable is read-only. This
    if (ClUseSWMRAnalysis) {
      assert(STI.has_value());
      if (const Value *V = getUnderlyingObject(Addr))
        if (const auto *GV = dyn_cast<GlobalVariable>(V)) {
          if (STI.value()->isSWMRGlobal(GV)) {
            LLVM_DEBUG(dbgs() << "Global variable " << GV->getName()
                              << " is read-only\n");
            continue;
          }
        }
    }

    LLVM_DEBUG(dbgs() << "Instruction instrumented\n");

    // Instrument this instruction.
    All.emplace_back(I);
    if (IsWrite) {
      // For read-before-write and compound instrumentation we only need one
      // write target, and we can override any previous entry if it exists.
      WriteTargets[Addr] = All.size() - 1;
    }
  }
  Local.clear();
}

DenseMap<Instruction *, size_t> ThreadSanitizer::createInstrIndexMap(
      SmallVectorImpl<InstructionInfo> &AllInstr) {
  DenseMap<Instruction *, size_t> InstToIndexInAll;
  for (size_t i = 0; i < AllInstr.size(); ++i)
    if (AllInstr[i].Inst) // Ensure the instruction hasn't been removed
      InstToIndexInAll[AllInstr[i].Inst] = i;
  return InstToIndexInAll;
}

const SmallPtrSetImpl<const BasicBlock *> *
ThreadSanitizer::getReverseReachable(const BasicBlock *BB) {
  const auto It = RevReachCache.find(BB);
  if (It != RevReachCache.end())
    return &It->second;

  SmallPtrSet<const BasicBlock *, 32> Reach;
  SmallVector<const BasicBlock *, 32> Worklist;
  Reach.insert(BB);
  Worklist.push_back(BB);
  while (!Worklist.empty())
    for (const BasicBlock *Pred : predecessors(Worklist.pop_back_val()))
      if (Reach.insert(Pred).second)
        Worklist.push_back(Pred);

  return &RevReachCache.try_emplace(BB, std::move(Reach)).first->second;
}

/// Condition (2) of redundancy: DomInst's instrumentation call must cover
/// everything CurrInst's would.
///
/// Both halves are needed. isMustAlias establishes that the two accesses start
/// at the same address but says nothing about their extent -- two
/// MemoryLocations with the same pointer and different sizes are still
/// MustAlias -- so a dominating one-byte write would otherwise be taken to
/// cover an eight-byte one, and races on the remaining seven bytes would go
/// unreported.
bool ThreadSanitizer::locationCovers(Instruction *DomInst,
                                     Instruction *CurrInst, AAResults *AA) {
  const MemoryLocation DomLoc = MemoryLocation::get(DomInst);
  const MemoryLocation CurrLoc = MemoryLocation::get(CurrInst);

  if (!DomLoc.Size.hasValue() || !CurrLoc.Size.hasValue() ||
      DomLoc.Size.isScalable() || CurrLoc.Size.isScalable())
    return false;
  if (DomLoc.Size.getValue().getFixedValue() <
      CurrLoc.Size.getValue().getFixedValue())
    return false;

  if (AA && AA->isMustAlias(DomLoc, CurrLoc))
    return true;

  // Alias analysis is conservative about some address computations that
  // nonetheless denote the same address by construction. If both pointers
  // reduce to the same SSA base with the same constant displacement, they are
  // the same address whatever AA managed to prove -- and since the base is one
  // SSA value, the two accesses see the same instance of it.
  const DataLayout &DL = DomInst->getModule()->getDataLayout();
  const unsigned Bits = DL.getIndexTypeSizeInBits(DomLoc.Ptr->getType());
  if (Bits != DL.getIndexTypeSizeInBits(CurrLoc.Ptr->getType()))
    return false;

  APInt DomOff(Bits, 0), CurrOff(Bits, 0);
  const Value *DomBase = DomLoc.Ptr->stripAndAccumulateConstantOffsets(
      DL, DomOff, /*AllowNonInbounds=*/true);
  const Value *CurrBase = CurrLoc.Ptr->stripAndAccumulateConstantOffsets(
      DL, CurrOff, /*AllowNonInbounds=*/true);
  return DomBase == CurrBase && DomOff == CurrOff;
}

static bool isTsanAtomic(const Instruction *I) {
  // TODO: Ask TTI whether synchronization scope is between threads.
  auto SSID = getAtomicSyncScopeID(I);
  if (!SSID)
    return false;
  if (isa<LoadInst>(I) || isa<StoreInst>(I))
    return *SSID != SyncScope::SingleThread;
  return true;
}

unsigned ThreadSanitizer::classifySyncEffect(const Instruction *Inst,
                                             const TargetLibraryInfo &TLI) {
  if (!Inst)
    return SYNC_NONE;

  // An inter-thread atomic or fence can establish happens-before in either
  // direction, so it blocks elimination both ways.
  if (isTsanAtomic(Inst))
    return SYNC_UNKNOWN;

  const auto *CB = dyn_cast<CallBase>(Inst);
  if (!CB)
    return SYNC_NONE;

  const Function *Callee = CB->getCalledFunction();

  // An indirect call could do anything, including not returning.
  if (!Callee)
    return SYNC_UNKNOWN | SYNC_MAY_NOT_RETURN;

  // Locks are the common case on a path between two accesses, and they are
  // directional -- see SyncEffect. Recognising them, rather than treating any
  // lock as opaque, is what lets dominance cross an acquire and post-dominance
  // cross a release. They are also modelled as returning: a lock that blocks
  // forever is a deadlock, not something the instrumentation reasons about.
  if (TargetLibraryInfo::isLockAcquireFunction(*Callee))
    return SYNC_ACQUIRE;
  if (TargetLibraryInfo::isLockReleaseFunction(*Callee))
    return SYNC_RELEASE;

  // Library and intrinsic calls we recognise: their synchronization behaviour
  // is tabulated and they return.
  LibFunc Func;
  if (TLI.getLibFunc(*Callee, Func))
    return TLI.isSyncFree(Func) ? SYNC_NONE : SYNC_UNKNOWN;

  if (Callee->isIntrinsic())
    return Intrinsic::isIntrinsicSyncFree(Callee->getIntrinsicID())
               ? SYNC_NONE
               : SYNC_UNKNOWN;

  // What is left is a call into code of this program. Whether it synchronizes
  // and whether it terminates are separate questions, and only the second one
  // needs the termination bit -- a callee that may synchronize is already
  // blocking both directions.
  const bool MayNotReturn =
      !CB->hasFnAttr(Attribute::WillReturn) &&
      !Callee->hasFnAttribute(Attribute::WillReturn) &&
      !(ModuleThreadSanitizerPass::SFI &&
        ModuleThreadSanitizerPass::SFI->isLoopFree(Callee));
  const unsigned Termination = MayNotReturn ? SYNC_MAY_NOT_RETURN : SYNC_NONE;

  if (Callee->hasFnAttribute(Attribute::NoSync) ||
      Callee->hasFnAttribute(Attribute::ReadNone))
    return Termination;

  // Interprocedural sync-freedom. This is where we go beyond what the NoSync
  // attribute alone can prove, and it accounts for a large share of the
  // elimination on real code. SFI answers false for anything it has no body
  // for, so an opaque external call still lands on SYNC_UNKNOWN below.
  if (ModuleThreadSanitizerPass::SFI &&
      ModuleThreadSanitizerPass::SFI->isSyncFree(Callee))
    return Termination;

  return Termination | SYNC_UNKNOWN;
}

bool ThreadSanitizer::isInstrDangerous(const Instruction *Inst,
                                       const TargetLibraryInfo &TLI) {
  return (classifySyncEffect(Inst, TLI) &
          (SYNC_ACQUIRE | SYNC_RELEASE | SYNC_UNKNOWN)) != SYNC_NONE;
}

/// Union of the synchronization effects over every execution path that can
/// carry the program from FirstInst to SecondInst, plus -- when the access we
/// intend to remove sits on a cycle -- the path from that access back to
/// itself.
///
/// Accumulating rather than stopping at the first synchronizing instruction is
/// what makes the lock refinement usable: a path holding only acquires is
/// still eliminable under dominance, and previously the scan gave up before it
/// could establish that.
unsigned ThreadSanitizer::scanPaths(Instruction *FirstInst,
                                    Instruction *SecondInst,
                                    Instruction *RemovedInst,
                                    const TargetLibraryInfo &TLI,
                                    const LoopInfo *LI, ScalarEvolution *SE,
                                    bool NeedTermination) {
  unsigned Effect = SYNC_NONE;
  DenseMap<const Loop *, bool> LoopTerminates;

  // A loop on the path only matters when the covering access comes later: if
  // the loop may spin forever, that access is never reached. Rather than
  // vetoing every loop, ask scalar evolution for a trip count -- a loop with a
  // computable backedge-taken count terminates.
  auto noteTermination = [&](const BasicBlock *BB) {
    if (!NeedTermination || ClPostDomAggressive || !LI)
      return;
    const Loop *L = LI->getLoopFor(BB);
    if (!L)
      return;
    const auto [It, Inserted] = LoopTerminates.try_emplace(L, false);
    if (Inserted)
      It->second =
          SE && !isa<SCEVCouldNotCompute>(SE->getBackedgeTakenCount(L));
    if (!It->second)
      Effect |= SYNC_MAY_NOT_RETURN;
  };

  auto scanRange = [&](BasicBlock::iterator B, BasicBlock::iterator E) {
    for (auto It = B; It != E; ++It)
      Effect |= classifySyncEffect(&*It, TLI);
  };

  auto scanWholeBlock = [&](const BasicBlock *BB) {
    noteTermination(BB);
    for (const Instruction &I : *BB)
      Effect |= classifySyncEffect(&I, TLI);
  };

  // Walk forward from Seeds, staying inside Bound and never re-entering Stop,
  // scanning each block in full.
  auto scanCone = [&](const BasicBlock *From, const BasicBlock *Stop,
                      const SmallPtrSetImpl<const BasicBlock *> *Bound) {
    SmallPtrSet<const BasicBlock *, 32> Visited;
    SmallVector<const BasicBlock *, 16> Worklist;
    auto enqueue = [&](const BasicBlock *BB) {
      if (BB != Stop && (!Bound || Bound->count(BB)) && Visited.insert(BB).second)
        Worklist.push_back(BB);
    };
    for (const BasicBlock *Succ : successors(From))
      enqueue(Succ);
    while (!Worklist.empty()) {
      const BasicBlock *BB = Worklist.pop_back_val();
      scanWholeBlock(BB);
      for (const BasicBlock *Succ : successors(BB))
        enqueue(Succ);
    }
  };

  BasicBlock *FirstBB = FirstInst->getParent();
  BasicBlock *SecondBB = SecondInst->getParent();

  if (FirstBB == SecondBB) {
    noteTermination(FirstBB);
    scanRange(std::next(FirstInst->getIterator()), SecondInst->getIterator());
  } else {
    noteTermination(FirstBB);
    noteTermination(SecondBB);
    // The suffix of the first block and the prefix of the second execute on
    // every path between the two accesses.
    scanRange(std::next(FirstInst->getIterator()), FirstBB->end());
    scanRange(SecondBB->begin(), SecondInst->getIterator());

    // Everything in between. Bounding the walk by the blocks that can actually
    // reach SecondBB keeps a diverging branch -- one that never reaches the
    // second access at all -- from vetoing the elimination.
    scanCone(FirstBB, SecondBB, getReverseReachable(SecondBB));
  }

  // Consecutive dynamic executions of the access we are about to remove. When
  // that access is on a cycle and its cover sits outside, the cover may run
  // once while the removed access runs many times; iterations after the first
  // are only covered if the path from the access back to itself is clear too.
  // Without this, a mutex released in a loop latch went unnoticed.
  BasicBlock *RemovedBB = RemovedInst->getParent();
  const SmallPtrSetImpl<const BasicBlock *> *ReachesRemoved =
      getReverseReachable(RemovedBB);
  const bool OnCycle = llvm::any_of(successors(RemovedBB),
                                    [&](const BasicBlock *Succ) {
                                      return ReachesRemoved->count(Succ);
                                    });
  if (OnCycle) {
    noteTermination(RemovedBB);
    for (const Instruction &I : *RemovedBB)
      if (&I != RemovedInst)
        Effect |= classifySyncEffect(&I, TLI);
    scanCone(RemovedBB, RemovedBB, ReachesRemoved);
  }

  return Effect;
}

template <bool IsPostDom>
void ThreadSanitizer::eliminateInstrByPrePostDominance(
    SmallVectorImpl<InstructionInfo> &AllInstr,
    const DominatorTreeBase<BasicBlock, IsPostDom> *DTBase, AAResults *AA,
    const TargetLibraryInfo &TLI, const LoopInfo *LI, ScalarEvolution *SE) {
  LLVM_DEBUG(dbgs() << "\n=== Starting " << (IsPostDom ? "post-" : "")
                    << "dominance-based analysis ===\n");
  assert(DTBase && "(Post)DominationTree must be provided");
  if (AllInstr.empty())
    return;

  const DenseMap<Instruction *, size_t> InstToIndexInAll =
      createInstrIndexMap(AllInstr);

  SmallVector<bool, 16> ToRemove(AllInstr.size(), false);

  // Coverage composes: if A covers B and B covers C, then A covers C, because
  // every path from A to C runs through B and both halves are clear. So an
  // access that has itself been eliminated may still stand in for a later one,
  // and we no longer lose eliminations to the order in which candidates happen
  // to be visited. Tracking the surviving root of each chain is what keeps two
  // accesses from covering each other into oblivion.
  SmallVector<size_t, 16> CoverRoot(AllInstr.size());
  for (size_t i = 0; i < AllInstr.size(); ++i)
    CoverRoot[i] = i;
  auto findRoot = [&CoverRoot](size_t I) {
    while (CoverRoot[I] != I) {
      CoverRoot[I] = CoverRoot[CoverRoot[I]];
      I = CoverRoot[I];
    }
    return I;
  };

  auto isVolatileAccess = [](const Instruction *I) {
    if (const auto *L = dyn_cast<LoadInst>(I))
      return L->isVolatile();
    if (const auto *S = dyn_cast<StoreInst>(I))
      return S->isVolatile();
    return false;
  };

  // Look for an instrumented access that (post-)dominates AllInstr[I] and
  // covers it. Returns the index of that access, if there is one.
  auto findCover = [&](size_t I) -> std::optional<size_t> {
    const InstructionInfo &CurrII = AllInstr[I];
    Instruction *CurrInst = CurrII.Inst;
    const BasicBlock *CurrBB = CurrInst->getParent();

    DomTreeNode *CurrDTNode = DTBase->getNode(const_cast<BasicBlock *>(CurrBB));
    if (!CurrDTNode)
      return std::nullopt;

    for (DomTreeNode *Node = CurrDTNode; Node && Node->getBlock();
         Node = Node->getIDom()) {
      BasicBlock *DomBB = Node->getBlock();

      // Within the access's own block, only instructions on the correct side
      // of it are candidates.
      auto StartIt = DomBB->begin();
      auto EndIt = DomBB->end();
      if (CurrBB == DomBB) {
        if (IsPostDom)
          StartIt = CurrInst->getIterator();
        else
          EndIt = CurrInst->getIterator();
      }

      for (auto InstIt = StartIt; InstIt != EndIt; ++InstIt) {
        Instruction *DomInst = &*InstIt;
        if (DomInst == CurrInst)
          continue;

        const auto It = InstToIndexInAll.find(DomInst);
        if (It == InstToIndexInAll.end())
          continue; // not an instrumented access
        const size_t DomIndex = It->second;
        if (findRoot(DomIndex) == I)
          continue; // would make the access cover itself

        const InstructionInfo &DomII = AllInstr[DomIndex];

        // With -tsan-distinguish-volatile the two emit different runtime
        // calls, so one cannot stand in for the other.
        if (ClDistinguishVolatile &&
            (isVolatileAccess(DomInst) || isVolatileAccess(CurrInst)))
          continue;

        // Condition (2): same location, and the cover is at least as wide.
        if (!locationCovers(DomInst, CurrInst, AA))
          continue;

        // Condition (3): a write covers a later read or write; a read covers
        // only a read, since a write may race with a remote read that the
        // covering read would not flag.
        if (!DomII.isWriteOperation() && CurrII.isWriteOperation())
          continue;

        // Condition (4): no synchronization on any path between them.
        Instruction *FirstInst = IsPostDom ? CurrInst : DomInst;
        Instruction *SecondInst = IsPostDom ? DomInst : CurrInst;
        const unsigned Effect =
            scanPaths(FirstInst, SecondInst, CurrInst, TLI, LI, SE,
                      /*NeedTermination=*/IsPostDom);
        if (Effect & (IsPostDom ? SYNC_BLOCKS_POSTDOM : SYNC_BLOCKS_DOM))
          continue;

        LLVM_DEBUG(dbgs() << "TSAN: Omitting instrumentation for " << *CurrInst
                          << " (covered by " << *DomInst << ")\n");
        return DomIndex;
      }
    }
    return std::nullopt;
  };

  unsigned RemovedCount = 0;
  for (size_t i = 0; i < AllInstr.size(); ++i) {
    if (ToRemove[i])
      continue;
    if (const std::optional<size_t> Cover = findCover(i)) {
      ToRemove[i] = true;
      CoverRoot[i] = findRoot(*Cover);
      ++RemovedCount;
    }
  }

  if (RemovedCount == 0)
    return;

  SmallVector<InstructionInfo, 8> NewAllInstr;
  NewAllInstr.reserve(AllInstr.size() - RemovedCount);
  for (size_t k = 0; k < AllInstr.size(); ++k)
    if (!ToRemove[k])
      NewAllInstr.push_back(AllInstr[k]);
  AllInstr.swap(NewAllInstr);

  if (IsPostDom)
    NumOmittedByPostDominance += RemovedCount;
  else
    NumOmittedByDominance += RemovedCount;

  LLVM_DEBUG(dbgs() << "=== " << (IsPostDom ? "Post-dominance" : "Dominance")
                    << " removed " << RemovedCount << " accesses ===\n");
}

void ThreadSanitizer::InsertRuntimeIgnores(Function &F) {
  InstrumentationIRBuilder IRB(F.getEntryBlock().getFirstNonPHI());
  IRB.CreateCall(TsanIgnoreBegin);
  EscapeEnumerator EE(F, "tsan_ignore_cleanup", ClHandleCxxExceptions);
  while (IRBuilder<> *AtExit = EE.Next()) {
    InstrumentationIRBuilder::ensureDebugInfo(*AtExit, F);
    AtExit->CreateCall(TsanIgnoreEnd);
  }
}

bool ThreadSanitizer::sanitizeFunction(
    Function &F, const TargetLibraryInfo &TLI,
    const std::optional<EscapeAnalysisInfo> &EAI,
    std::optional<EscapeAnalysisGlobalInfo *> EAIGlobal,
    std::optional<LockOwnershipInfo *> LOI,
    std::optional<SingleThreadedInfo *> STI, DominatorTree *DT,
    PostDominatorTree *PDT, AAResults *AA, LoopInfo *LI, AssumptionCache *AC,
    ScalarEvolution *SE) {
  LLVM_DEBUG(dbgs() << "\n%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%"
                       "%%%%%%%%%%%%%%%%%\n"
    "%%%%%%%%%%%%%%%%%%%% Func " << F.getName() << "\t%%%%%%%%%%%%%%%%%%%%%%\n"
    "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n");
  M = F.getParent();
  Func = &F;
  RevReachCache.clear();

  // This is required to prevent instrumenting call to __tsan_init from within
  // the module constructor.
  if (F.getName() == kTsanModuleCtorName)
    return false;
  // Naked functions can not have prologue/epilogue
  // (__tsan_func_entry/__tsan_func_exit) generated, so don't instrument them at
  // all.
  if (F.hasFnAttribute(Attribute::Naked))
    return false;

  // __attribute__(disable_sanitizer_instrumentation) prevents all kinds of
  // instrumentation.
  if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation))
    return false;

  // 0. Skip instrumentation for functions that are proven to be single-threaded
  if (ClUseSingleThreadedAnalysis) {
    assert(STI.has_value());
    if (STI.value()->isSingleThreaded(&F)) {
      LLVM_DEBUG(
          dbgs() << "Function is single-threaded, skipping instrumentation: "
                 << F.getName() << "\n");
      return false;
    }
  }

  initialize(*F.getParent(), TLI);
  SmallVector<InstructionInfo, 8> AllLoadsAndStores;
  SmallVector<Instruction*, 8> LocalLoadsAndStores;
  SmallVector<Instruction*, 8> AtomicAccesses;
  SmallVector<Instruction*, 8> MemIntrinCalls;
  SmallVector<CallInst*, 8> InterceptedCalls;

  bool Res = false;
  bool HasCalls = false;
  bool SanitizeFunction = F.hasFnAttribute(Attribute::SanitizeThread);
  const DataLayout &DL = F.getParent()->getDataLayout();

  ///////////////////////////////////////////////////////////////////////////////
  // List of instructions (function calls) to delete
  SmallVector<CallInst*, 8> EnableDisableFuncCleanupList;

  // Counter for considering nesting __tsan_disable/__tsan_enable
  int enableDisableTSanCntr = 0;

  // Traverse all instructions, collect loads/stores/returns, check for calls.
  for (auto &BB : F) {
    LLVM_DEBUG(dbgs() << "\nInstrumenting BB: " << BB.getName() << "\n");
    for (auto &Inst : BB) {
      LLVM_DEBUG(dbgs() << "Instrumenting I: " << Inst << "\n");
            ///////////////////////////////////////////////////////////////////////////////
      // This code is for disabling/enabling instrumentation
      // for specific code section
      //
      // Check whether we encountered TSan disable/enable intrinsics
      if (auto *CI = dyn_cast<CallInst>(&Inst)) {
        Function *CalledFunc = CI->getCalledFunction();
        if (CalledFunc) {
          // errs() << "CalledFunc: " << CalledFunc->getName() << "\n";
          if (CalledFunc->getName() == "__tsan_disable") {
            enableDisableTSanCntr++;
            EnableDisableFuncCleanupList.push_back(CI);
            continue;
          }
          if (CalledFunc->getName() == "__tsan_enable") {
            enableDisableTSanCntr--;
            EnableDisableFuncCleanupList.push_back(CI);
            continue;
          }
        }
      }

      if (enableDisableTSanCntr > 0)
        continue;
      ///////////////////////////////////////////////////////////////////////////////

      // Skip instructions inserted by another instrumentation.
      if (Inst.hasMetadata(LLVMContext::MD_nosanitize))
        continue;
      if (isTsanAtomic(&Inst))
        AtomicAccesses.push_back(&Inst);
      else if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst))
        LocalLoadsAndStores.push_back(&Inst);
      else if ((isa<CallInst>(Inst) && !isa<DbgInfoIntrinsic>(Inst)) ||
               isa<InvokeInst>(Inst)) {
        bool IsIntr = false, IsInterc = false;
        if (CallInst *CI = dyn_cast<CallInst>(&Inst))
          maybeMarkSanitizerLibraryCallNoBuiltin(CI, &TLI);
        if (isa<MemIntrinsic>(Inst)) {
          MemIntrinCalls.push_back(&Inst);
          IsIntr = true;
        }

        if (auto *CI = dyn_cast<CallInst>(&Inst)) {
          if (Function *Callee = CI->getCalledFunction()) {
            if (Callee->getName() == "strcmp" ||
                Callee->getName() == "memchr" ||
                Callee->getName() == "strlen") {
              InterceptedCalls.push_back(CI);
              IsInterc = true;
            }
          }
        }

        if (!IsIntr && !IsInterc)
          HasCalls = true;
        chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores,
                                       TLI, DL, EAI, EAIGlobal, LOI, STI);
      }
    }
    chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores, TLI,
                                   DL, EAI, EAIGlobal, LOI, STI);
  }

  if (ClUseDominanceAnalysis) {
    eliminateInstrByPrePostDominance(AllLoadsAndStores, DT, AA, TLI, LI, SE);
    eliminateInstrByPrePostDominance(AllLoadsAndStores, PDT, AA, TLI, LI, SE);
  } else {
    if (ClUseDominanceAnalysisDom)
      eliminateInstrByPrePostDominance(AllLoadsAndStores, DT, AA, TLI, LI, SE);
    if (ClUseDominanceAnalysisPostDom)
      eliminateInstrByPrePostDominance(AllLoadsAndStores, PDT, AA, TLI, LI, SE);
  }

  //////////////////////////////////////////////////////////////////////////////
  // This is for disabling/enabling TSan instrumentation
  // Disable for now check of matching
  /* if (disableEnableNesting != 0) {
    // Error: __tsan_enable without __tsan_disable
    report_fatal_error("Unmatched __tsan_enable__ call in function " +
                       F.getName());
  } */

  // Erase all __tsan_disable/enable functions
  for (auto *CI: EnableDisableFuncCleanupList)
    CI->eraseFromParent();
  //////////////////////////////////////////////////////////////////////////////

  // We have collected all loads and stores.
  // FIXME: many of these accesses do not need to be checked for races
  // (e.g. variables that do not escape, etc).

  if (ClInstrumentMemIntrinsics && SanitizeFunction)
    for (auto *Inst : MemIntrinCalls) {
      Res |= instrumentMemIntrinsic(Inst, TLI, EAIGlobal);
    }

  for (CallInst *CI: InterceptedCalls)
    Res |= instrumentInterceptedCalls(CI, TLI, EAIGlobal);

  // Instrument memory accesses only if we want to report bugs in the function.
  if (ClInstrumentMemoryAccesses && SanitizeFunction)
    for (const auto &II : AllLoadsAndStores) {
      Res |= instrumentLoadOrStore(II, DL);
    }

  // Instrument atomic memory accesses in any case (they can be used to
  // implement synchronization).
  if (ClInstrumentAtomics)
    for (auto *Inst : AtomicAccesses) {
      Res |= instrumentAtomic(Inst, DL);
    }

  if (F.hasFnAttribute("sanitize_thread_no_checking_at_run_time")) {
    assert(!F.hasFnAttribute(Attribute::SanitizeThread));
    if (HasCalls)
      InsertRuntimeIgnores(F);
  }

  // Instrument function entry/exit points if there were instrumented accesses.
  if ((Res || HasCalls) && ClInstrumentFuncEntryExit) {
    InstrumentationIRBuilder IRB(F.getEntryBlock().getFirstNonPHI());
    Value *ReturnAddress = IRB.CreateCall(
        Intrinsic::getDeclaration(F.getParent(), Intrinsic::returnaddress),
        IRB.getInt32(0));
    IRB.CreateCall(TsanFuncEntry, ReturnAddress);

    EscapeEnumerator EE(F, "tsan_cleanup", ClHandleCxxExceptions);
    while (IRBuilder<> *AtExit = EE.Next()) {
      InstrumentationIRBuilder::ensureDebugInfo(*AtExit, F);
      AtExit->CreateCall(TsanFuncExit, {});
    }
    Res = true;
  }
  return Res;
}

Value *ThreadSanitizer::checkActiveThreadCount(IRBuilderBase &IRB, Module &M) {
  // External global maintained by the TSan runtime.
  // If it's > 1, we may have multiple threads and should do the expensive check.
  LLVMContext &Ctx = M.getContext();
  Type *I32Ty = Type::getInt32Ty(Ctx);

  // Model this as an atomic i32 so the generated load can be marked atomic.
  // This matches the runtime's use of sanitizer_common atomics.
  Type *AtomicI32Ty = I32Ty;
  auto *ThreadCount = cast<GlobalVariable>(
      M.getOrInsertGlobal("__tsan_active_thread_count", AtomicI32Ty));

  LoadInst *Cnt = IRB.CreateLoad(AtomicI32Ty, ThreadCount,
                                "tsan.active_thread_count");
  // Runtime updates this concurrently; use an atomic acquire load to prevent
  // undesired reordering around the check.
  Cnt->setAtomic(AtomicOrdering::Monotonic);
  // Cnt->setAtomic(AtomicOrdering::Acquire);

  return IRB.CreateICmpUGT(Cnt, ConstantInt::get(I32Ty, 1), "tsan.mt");
}

bool ThreadSanitizer::instrumentLoadOrStore(const InstructionInfo &II,
                                            const DataLayout &DL) {
  InstrumentationIRBuilder IRB(II.Inst);
  const bool IsWrite = isa<StoreInst>(*II.Inst);
  Value *Addr = IsWrite ? cast<StoreInst>(II.Inst)->getPointerOperand()
                        : cast<LoadInst>(II.Inst)->getPointerOperand();
  Type *OrigTy = getLoadStoreType(II.Inst);

  // swifterror memory addresses are mem2reg promoted by instruction selection.
  // As such they cannot have regular uses like an instrumentation function and
  // it makes no sense to track them as memory.
  if (Addr->isSwiftError())
    return false;

  int Idx = getMemoryAccessFuncIndex(OrigTy, Addr, DL);
  if (Idx < 0)
    return false;
  if (IsWrite && isVtableAccess(II.Inst)) {
    LLVM_DEBUG(dbgs() << "  VPTR : " << *II.Inst << "\n");
    Value *StoredValue = cast<StoreInst>(II.Inst)->getValueOperand();
    // StoredValue may be a vector type if we are storing several vptrs at once.
    // In this case, just take the first element of the vector since this is
    // enough to find vptr races.
    if (isa<VectorType>(StoredValue->getType()))
      StoredValue = IRB.CreateExtractElement(
          StoredValue, ConstantInt::get(IRB.getInt32Ty(), 0));
    if (StoredValue->getType()->isIntegerTy())
      StoredValue = IRB.CreateIntToPtr(StoredValue, IRB.getPtrTy());
    // Call TsanVptrUpdate.
    IRB.CreateCall(TsanVptrUpdate, {Addr, StoredValue});
    NumInstrumentedVtableWrites++;
    return true;
  }
  if (!IsWrite && isVtableAccess(II.Inst)) {
    IRB.CreateCall(TsanVptrLoad, Addr);
    NumInstrumentedVtableReads++;
    return true;
  }

  const Align Alignment = IsWrite ? cast<StoreInst>(II.Inst)->getAlign()
                                  : cast<LoadInst>(II.Inst)->getAlign();
  const bool IsCompoundRW =
      ClCompoundReadBeforeWrite && (II.Flags & InstructionInfo::kCompoundRW);
  const bool IsVolatile = ClDistinguishVolatile &&
                          (IsWrite ? cast<StoreInst>(II.Inst)->isVolatile()
                                   : cast<LoadInst>(II.Inst)->isVolatile());
  assert((!IsVolatile || !IsCompoundRW) && "Compound volatile invalid!");

  const uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  FunctionCallee OnAccessFunc = nullptr;
  if (Alignment >= Align(8) || (Alignment.value() % (TypeSize / 8)) == 0) {
    if (IsCompoundRW)
      OnAccessFunc = TsanCompoundRW[Idx];
    else if (IsVolatile)
      OnAccessFunc = IsWrite ? TsanVolatileWrite[Idx] : TsanVolatileRead[Idx];
    else
      OnAccessFunc = IsWrite ? TsanWrite[Idx] : TsanRead[Idx];
  } else {
    if (IsCompoundRW)
      OnAccessFunc = TsanUnalignedCompoundRW[Idx];
    else if (IsVolatile)
      OnAccessFunc = IsWrite ? TsanUnalignedVolatileWrite[Idx]
                             : TsanUnalignedVolatileRead[Idx];
    else
      OnAccessFunc = IsWrite ? TsanUnalignedWrite[Idx] : TsanUnalignedRead[Idx];
  }

  if (ClTsanUseActiveThreadCountFastPath) {
    // Fast-path: if there's only one active thread, skip calling into TSan.
    Value *DoCheck = checkActiveThreadCount(IRB, *II.Inst->getModule());
    Instruction *InsertPt = II.Inst;
    Instruction *ThenTerm = nullptr;
    Instruction *ElseTerm = nullptr;
    SplitBlockAndInsertIfThenElse(DoCheck, InsertPt, &ThenTerm, &ElseTerm);
    IRB.SetInsertPoint(ThenTerm);
  }
  IRB.CreateCall(OnAccessFunc, Addr);

  if (IsCompoundRW || IsWrite)
    NumInstrumentedWrites++;
  if (IsCompoundRW || !IsWrite)
    NumInstrumentedReads++;
  return true;
}

static ConstantInt *createOrdering(IRBuilder<> *IRB, AtomicOrdering ord) {
  uint32_t v = 0;
  switch (ord) {
    case AtomicOrdering::NotAtomic:
      llvm_unreachable("unexpected atomic ordering!");
    case AtomicOrdering::Unordered:              [[fallthrough]];
    case AtomicOrdering::Monotonic:              v = 0; break;
    // Not specified yet:
    // case AtomicOrdering::Consume:                v = 1; break;
    case AtomicOrdering::Acquire:                v = 2; break;
    case AtomicOrdering::Release:                v = 3; break;
    case AtomicOrdering::AcquireRelease:         v = 4; break;
    case AtomicOrdering::SequentiallyConsistent: v = 5; break;
  }
  return IRB->getInt32(v);
}

void ThreadSanitizer::disableInterceptorForInstr(
    Instruction *I, InstrumentationIRBuilder &IRB) {
  // Save and restore rather than forcing the flag back on afterwards: the
  // guarded call may sit inside a region where interceptors were already
  // disabled, and storing true would re-arm them early.
  Value *Saved = IRB.CreateLoad(IRB.getInt1Ty(), InterceptorEnabled,
                                "tsan.interceptors.saved");
  IRB.CreateStore(IRB.getInt1(false), InterceptorEnabled);
  IRB.SetInsertPoint(++BasicBlock::iterator(I));
  IRB.CreateStore(Saved, InterceptorEnabled);
}

static bool
isPointerEscaped(Value *Ptr, Instruction *I, const TargetLibraryInfo &TLI,
                 std::optional<EscapeAnalysisGlobalInfo *> EAIGlobal) {
  if (EAIGlobal.has_value()) {
    EscReasonTy EscReason;
    return EAIGlobal.value()->isEscapedUndrlObjOrPointee(
        Ptr, TLI, I->getParent(), EscReason);
  }
  return true;
}

bool ThreadSanitizer::instrumentInterceptedCalls(
    CallInst *CI, const TargetLibraryInfo &TLI,
    std::optional<EscapeAnalysisGlobalInfo *> EAIGlobal) {
  // Check which intercepted function is being called
  // LLVM_DEBUG(dbgs() << "Check " << *CI << "\n");

  Function *Callee = CI->getCalledFunction();
  bool ArePointersEscaped = true;

  // Check if pointers passed to the function escape
  if (Callee->getName() == "strcmp" || Callee->getName() == "memchr") {
    if (!isPointerEscaped(CI->getArgOperand(0), CI, TLI, EAIGlobal) &&
        !isPointerEscaped(CI->getArgOperand(1), CI, TLI, EAIGlobal)) {
      ArePointersEscaped = false;
    }
  } else if (Callee->getName() == "strlen") {
    if (!isPointerEscaped(CI->getArgOperand(0), CI, TLI, EAIGlobal))
      ArePointersEscaped = false;
  }

  // If none of the arguments escape, disable the interceptor
  if (!ArePointersEscaped) {
    LLVM_DEBUG(dbgs() << "Call does not escape any pointers\n");
    InstrumentationIRBuilder IRB(CI);
    disableInterceptorForInstr(CI, IRB);
    return false;
  }
  return true;
}

// So, we either need to ensure the intrinsic is not inlined, or instrument it.
// We do not instrument memset/memmove/memcpy intrinsics (too complicated),
// instead we simply replace them with regular function calls, which are then
// intercepted by the run-time.
// Since tsan is running after everyone else, the calls should not be
// replaced back with intrinsics. If that becomes wrong at some point,
// we will need to call e.g. __tsan_memset to avoid the intrinsics.
bool ThreadSanitizer::instrumentMemIntrinsic(
    Instruction *I, const TargetLibraryInfo &TLI,
    std::optional<EscapeAnalysisGlobalInfo *> EAIGlobal) {
  InstrumentationIRBuilder IRB(I);
  LLVM_DEBUG(dbgs() << "Instrumenting MemIntrinsic: " << *I << "\n");

  if (MemSetInst *M = dyn_cast<MemSetInst>(I)) {
    Value *Cast1 =
        IRB.CreateIntCast(M->getArgOperand(1), IRB.getInt32Ty(), false);
    Value *Cast2 = IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false);

    // Check if pointer is not escape
    if (!isPointerEscaped(M->getArgOperand(0), I, TLI, EAIGlobal)) {
      LLVM_DEBUG(dbgs() << "MemIntrinsic does not escape any pointers\n");
      disableInterceptorForInstr(I, IRB);
      return false;
    }
    LLVM_DEBUG(dbgs() << "MemIntrinsic escapes pointers\n");

    IRB.CreateCall(
        MemsetFn,
        {M->getArgOperand(0),
         Cast1,
         Cast2});
    I->eraseFromParent();
    // The intrinsic was replaced by an intercepted call, so the function does
    // now carry instrumentation. Reporting false here left functions whose
    // only instrumentation was a memory intrinsic without
    // __tsan_func_entry/__tsan_func_exit, dropping their frame from every race
    // report raised inside the interceptor.
    return true;
  } else if (MemTransferInst *M = dyn_cast<MemTransferInst>(I)) {
    // Not clear why, but the test signal_thread_sigctx_race.cpp fails if
    // we don't instrument memcpy. So this version works:
    // define internal noundef i32 @_ZL9do_selectv()
    //   ...
    //   %tvs = alloca %struct.timeval, align 8
    //   ...
    //   %1 = call ptr @__tsan_memcpy(ptr %tvs,
    //                                ptr @__const._ZL9do_selectv.tvs, i64 16)
    //
    // but this one doesn't work
    // call void @llvm.memcpy.p0.p0.i64(ptr align 8 %tvs,
    //                                  ptr align 8 @__const._ZL9do_selectv.tvs,
    //                                  i64 16, i1 false)

    // Check if the first argument of M is '%tvs = alloca %struct.timeval'
    bool TimevalCaseFlag = false;
    if (const auto *Alloca = dyn_cast<AllocaInst>(M->getArgOperand(0))) {
      if (Alloca->getAllocatedType()->isStructTy()) {
        if (const auto *Struct = cast<StructType>(Alloca->getAllocatedType());
            Struct->hasName() && (Struct->getName() == "struct.timeval")) {
          LLVM_DEBUG(dbgs()
                     << "First argument is an allocation of struct.timeval\n");
          TimevalCaseFlag = true;
        }
      }
    }

    //
    // Check if pointers are not escape
    if (!TimevalCaseFlag &&
        !isPointerEscaped(M->getArgOperand(0), I, TLI, EAIGlobal) &&
        !isPointerEscaped(M->getArgOperand(1), I, TLI, EAIGlobal)) {
      disableInterceptorForInstr(I, IRB);
      return false;
    }

    IRB.CreateCall(
        isa<MemCpyInst>(M) ? MemcpyFn : MemmoveFn,
        {M->getArgOperand(0),
         M->getArgOperand(1),
         IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false)});
    I->eraseFromParent();
    return true;
  }
  return false;
}

// Both llvm and ThreadSanitizer atomic operations are based on C++11/C1x
// standards.  For background see C++11 standard.  A slightly older, publicly
// available draft of the standard (not entirely up-to-date, but close enough
// for casual browsing) is available here:
// http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2011/n3242.pdf
// The following page contains more background information:
// http://www.hpl.hp.com/personal/Hans_Boehm/c++mm/

bool ThreadSanitizer::instrumentAtomic(Instruction *I, const DataLayout &DL) {
  InstrumentationIRBuilder IRB(I);
  if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
    Value *Addr = LI->getPointerOperand();
    Type *OrigTy = LI->getType();
    int Idx = getMemoryAccessFuncIndex(OrigTy, Addr, DL);
    if (Idx < 0)
      return false;
    Value *Args[] = {Addr,
                     createOrdering(&IRB, LI->getOrdering())};
    Value *C = IRB.CreateCall(TsanAtomicLoad[Idx], Args);
    Value *Cast = IRB.CreateBitOrPointerCast(C, OrigTy);
    I->replaceAllUsesWith(Cast);
  } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    Value *Addr = SI->getPointerOperand();
    int Idx =
        getMemoryAccessFuncIndex(SI->getValueOperand()->getType(), Addr, DL);
    if (Idx < 0)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Value *Args[] = {Addr,
                     IRB.CreateBitOrPointerCast(SI->getValueOperand(), Ty),
                     createOrdering(&IRB, SI->getOrdering())};
    IRB.CreateCall(TsanAtomicStore[Idx], Args);
    SI->eraseFromParent();
  } else if (AtomicRMWInst *RMWI = dyn_cast<AtomicRMWInst>(I)) {
    Value *Addr = RMWI->getPointerOperand();
    int Idx =
        getMemoryAccessFuncIndex(RMWI->getValOperand()->getType(), Addr, DL);
    if (Idx < 0)
      return false;
    FunctionCallee F = TsanAtomicRMW[RMWI->getOperation()][Idx];
    if (!F)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Value *Val = RMWI->getValOperand();
    Value *Args[] = {Addr, IRB.CreateBitOrPointerCast(Val, Ty),
                     createOrdering(&IRB, RMWI->getOrdering())};
    Value *C = IRB.CreateCall(F, Args);
    I->replaceAllUsesWith(IRB.CreateBitOrPointerCast(C, Val->getType()));
    I->eraseFromParent();
  } else if (AtomicCmpXchgInst *CASI = dyn_cast<AtomicCmpXchgInst>(I)) {
    Value *Addr = CASI->getPointerOperand();
    Type *OrigOldValTy = CASI->getNewValOperand()->getType();
    int Idx = getMemoryAccessFuncIndex(OrigOldValTy, Addr, DL);
    if (Idx < 0)
      return false;
    const unsigned ByteSize = 1U << Idx;
    const unsigned BitSize = ByteSize * 8;
    Type *Ty = Type::getIntNTy(IRB.getContext(), BitSize);
    Value *CmpOperand =
      IRB.CreateBitOrPointerCast(CASI->getCompareOperand(), Ty);
    Value *NewOperand =
      IRB.CreateBitOrPointerCast(CASI->getNewValOperand(), Ty);
    Value *Args[] = {Addr,
                     CmpOperand,
                     NewOperand,
                     createOrdering(&IRB, CASI->getSuccessOrdering()),
                     createOrdering(&IRB, CASI->getFailureOrdering())};
    CallInst *C = IRB.CreateCall(TsanAtomicCAS[Idx], Args);
    Value *Success = IRB.CreateICmpEQ(C, CmpOperand);
    Value *OldVal = C;
    if (Ty != OrigOldValTy) {
      // The value is a pointer, so we need to cast the return value.
      OldVal = IRB.CreateIntToPtr(C, OrigOldValTy);
    }

    Value *Res =
      IRB.CreateInsertValue(PoisonValue::get(CASI->getType()), OldVal, 0);
    Res = IRB.CreateInsertValue(Res, Success, 1);

    I->replaceAllUsesWith(Res);
    I->eraseFromParent();
  } else if (FenceInst *FI = dyn_cast<FenceInst>(I)) {
    Value *Args[] = {createOrdering(&IRB, FI->getOrdering())};
    FunctionCallee F = FI->getSyncScopeID() == SyncScope::SingleThread
                           ? TsanAtomicSignalFence
                           : TsanAtomicThreadFence;
    IRB.CreateCall(F, Args);
    FI->eraseFromParent();
  }
  return true;
}

int ThreadSanitizer::getMemoryAccessFuncIndex(Type *OrigTy, Value *Addr,
                                              const DataLayout &DL) {
  assert(OrigTy->isSized());
  if (OrigTy->isScalableTy()) {
    // FIXME: support vscale.
    return -1;
  }
  uint32_t TypeSize = DL.getTypeStoreSizeInBits(OrigTy);
  if (TypeSize != 8  && TypeSize != 16 &&
      TypeSize != 32 && TypeSize != 64 && TypeSize != 128) {
    NumAccessesWithBadSize++;
    // Ignore all unusual sizes.
    return -1;
  }
  size_t Idx = llvm::countr_zero(TypeSize / 8);
  assert(Idx < kNumberOfAccessSizes);
  return Idx;
}

//===----------------------------------------------------------------------===//
// Sync-free analysis (needed by eliminateDominatingInstr)
//===----------------------------------------------------------------------===//

/// SyncFreeInfo analyzes whether functions are "sync-free" - meaning they have
/// no operations that could cause synchronization between threads. This
/// includes:
/// - No explicit synchronization operations (locks, memory fences etc)
/// - No atomic operations
/// - No calls to functions that are not sync-free
/// This analysis works on the whole module by traversing the call graph
/// bottom-up. Results are used to optimize instrumentation by skipping thread
/// checks in sync-free code.
///
/// @param M_ The LLVM module to analyze
/// @param CG_ The call graph for the module
SyncFreeInfo::SyncFreeInfo(Module &M_, CallGraph &CG_,
                           AnalysisManager<Function> &AM_)
    : M(M_), CG(CG_), AM(AM_) {
  findFunsContainsLoops();
  LLVM_DEBUG(dbgs() << "\n-----------------------------------------------\n"
                    << "=== Starting sync-free analysis ===\n"
                    << "Module: " << M.getName() << "\n";);

  // A function we have a body for starts out assumed sync-free and is proven
  // dangerous below. A declaration has no body to prove anything from: it is
  // defined in another translation unit and may lock, unlock or spawn threads,
  // so it stays dangerous. Initializing declarations to false made every call
  // to an invisible external function look synchronization-free, which let
  // dominance elimination remove instrumentation across it.
  for (const Function &F : M)
    IsFuncDangerousGlobal[&F] = F.isDeclaration();

  // Analyze strongly connected components (SCCs) of the call graph in reverse
  // topological order. This ensures we process callees before their callers:
  // 1. Start from leaf functions (SCCs with no outgoing edges)
  // 2. Mark functions as dangerous if they contain sync operations
  // 3. Propagate results up through caller-callee relationships
  // 4. Handle recursive calls by processing SCCs as a unit until fixpoint
  for (auto SCCI = scc_begin(&CG); !SCCI.isAtEnd(); ++SCCI) {
    const auto &CurrentSCC = *SCCI;

    // Init by non-dangerous
    for (const CallGraphNode *CGNode : CurrentSCC) {
      const Function *F = CGNode->getFunction();
      if (!F || F->isDeclaration())
        continue;
      IsFuncDangerousGlobal[F] = false;
    }

    // Check if any function in the SCC calls contains an intrinsically
    // dangerous instruction.
    // do {
    // IsFuncDangerousSCCOld = IsFuncDangerousGlobal;
    bool AnySCCFuncDangerous = false;
    for (const CallGraphNode *CGNode : CurrentSCC) {
      Function *F = CGNode->getFunction();
      if (!F || F->isDeclaration())
        continue;
      LLVM_DEBUG(dbgs() << "\nChecking function: " << F->getName() << "\n");

      // If it's not sync-free already, it will never become sync-free
      if (const auto It = IsFuncDangerousGlobal.find(F);
          It != IsFuncDangerousGlobal.end() && It->second)
        continue;

      for (const Instruction &I : instructions(F)) {
        LLVM_DEBUG(dbgs() << "Checking instruction: " << I << "\n");
        // Check if it's a call, and we already know the status of the callee
        if (const CallInst *CI = dyn_cast<CallInst>(&I)) {
          if (const Function *Callee = CI->getCalledFunction()) {
            // First check in FuncsInSSCMap (current SCC)
            if (std::any_of(CurrentSCC.begin(), CurrentSCC.end(),
                            [&Callee](const CallGraphNode *CGN) {
                              return CGN->getFunction() == Callee;
                            }))
              continue;

            // If not in the current SCC, check global IsFuncDangerousMap
            if (const auto It = IsFuncDangerousGlobal.find(Callee);
                It != IsFuncDangerousGlobal.end() && It->second) {
              AnySCCFuncDangerous = true;
              break;
            }
          }
        }

        if (ThreadSanitizer::isInstrDangerous(
                &I, AM.getResult<TargetLibraryAnalysis>(*F))) {
          AnySCCFuncDangerous = true;
          break;
        }
      }
    }

    // If any function in SCC is dangerous, mark all functions in SCC as
    // dangerous
    if (AnySCCFuncDangerous) {
      for (const CallGraphNode *CGNode : CurrentSCC) {
        const Function *F = CGNode->getFunction();
        if (!F || F->isDeclaration())
          continue;
        IsFuncDangerousGlobal[F] = true;
      }
    }
  }

  LLVM_DEBUG(dbgs() << "\n=== Completed sync-free analysis ===\n";
             dbgs() << "\nResults of sync-free analysis:\n";
             for (const auto &Pair : IsFuncDangerousGlobal)
               // if (!Pair.first->isDeclaration())
               dbgs() << "Function " << Pair.first->getName() << ": "
                      << (Pair.second ? "contains sync operations"
                                      : "is sync-free")
                      << "\n";
             dbgs()
             << "-----------------------------------------------\n\n";);
}

void SyncFreeInfo::findFunsContainsLoops() {
  // As above: without a body we cannot claim the function terminates, so treat
  // a declaration as if it looped.
  for (const Function &F : M)
    IsFuncContainsLoops[&F] = F.isDeclaration();

  // Analyze strongly connected components (SCCs) of the call graph in reverse
  // topological order. This ensures we process callees before their callers:
  // 1. Start from leaf functions (SCCs with no outgoing edges)
  // 2. Mark functions as dangerous if they contain sync operations
  // 3. Propagate results up through caller-callee relationships
  // 4. Handle recursive calls by processing SCCs as a unit until fixpoint
  for (auto SCCI = scc_begin(&CG); !SCCI.isAtEnd(); ++SCCI) {
    const auto &CurrentSCC = *SCCI;

    // Init by non-dangerous
    for (const CallGraphNode *CGNode : CurrentSCC) {
      const Function *F = CGNode->getFunction();
      if (!F || F->isDeclaration())
        continue;
      IsFuncContainsLoops[F] = false;
    }

    // Check if any function in the SCC calls contains an intrinsically
    // dangerous instruction.
    // do {
    // IsFuncDangerousSCCOld = IsFuncDangerousGlobal;
    bool AnySCCFuncContainsLoop = false;
    for (const CallGraphNode *CGNode : CurrentSCC) {
      Function *F = CGNode->getFunction();
      if (!F || F->isDeclaration())
        continue;
      LLVM_DEBUG(dbgs() << "\nChecking function: " << F->getName() << "\n");
      const LoopInfo &LI = AM.getResult<LoopAnalysis>(*F);

      // If it's not sync-free already, it will never become sync-free
      if (const auto It = IsFuncContainsLoops.find(F);
          It != IsFuncContainsLoops.end() && It->second)
        continue;

      for (const Instruction &I : instructions(F)) {
        LLVM_DEBUG(dbgs() << "Checking instruction: " << I << "\n");
        // Check if it's a call, and we already know the status of the callee
        if (const CallInst *CI = dyn_cast<CallInst>(&I)) {
          if (const Function *Callee = CI->getCalledFunction()) {
            // First check in FuncsInSSCMap (current SCC)
            if (std::any_of(CurrentSCC.begin(), CurrentSCC.end(),
                            [&Callee](const CallGraphNode *CGN) {
                              return CGN->getFunction() == Callee;
                            }))
              continue;

            // If not in the current SCC, check global IsFuncDangerousMap
            if (const auto It = IsFuncContainsLoops.find(Callee);
                It != IsFuncContainsLoops.end() && It->second) {
              AnySCCFuncContainsLoop = true;
              break;
            }
          }
        }

        if (LI.getLoopFor(I.getParent()) != nullptr) {
          AnySCCFuncContainsLoop = true;
          break;
        }
      }
    }

    // If any function in SCC is dangerous, mark all functions in SCC as
    // dangerous
    if (AnySCCFuncContainsLoop) {
      for (const CallGraphNode *CGNode : CurrentSCC) {
        const Function *F = CGNode->getFunction();
        if (!F || F->isDeclaration())
          continue;
        IsFuncContainsLoops[F] = true;
      }
    }
  }
}
