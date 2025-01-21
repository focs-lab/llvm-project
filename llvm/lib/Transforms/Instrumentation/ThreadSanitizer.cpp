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
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/MDBuilder.h"
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
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"

using namespace llvm;

#define DEBUG_TYPE "tsan"
#define MONITOR_CALL_HANDLERS 0
#define MONITOR_CALL_ATOMIC_HANDLERS 0
#define MONITOR_CALL_LOGGER 1
#define MONITOR_SAVE_INFO_FOR_CALLS 0
#define MONITOR_SAMPLING 1
#define MONITOR_DEBUG 0
#define MONITOR_USE_LOCAL_IDX 0

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
static cl::opt<bool> ClInstrumentAtomics("tsan-instrument-atomics",
                                         cl::init(true),
                                         cl::desc("Instrument atomics"),
                                         cl::Hidden);
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

const char kTsanModuleCtorName[] = "tsan.module_ctor";
const char kTsanInitName[] = "__tsan_init";

namespace {

/// ThreadSanitizer: instrument the code in module to find races.
///
/// Instantiating ThreadSanitizer inserts the tsan runtime library API function
/// declarations into the module if they don't exist already. Instantiating
/// ensures the __tsan_init function is in the list of global constructors for
/// the module.
struct ThreadSanitizer {
  ThreadSanitizer() {
    // Check options and warn user.
    if (ClInstrumentReadBeforeWrite && ClCompoundReadBeforeWrite) {
      errs()
          << "warning: Option -tsan-compound-read-before-write has no effect "
             "when -tsan-instrument-read-before-write is set.\n";
    }
  }

  bool sanitizeFunction(Function &F, FunctionAnalysisManager &FAM, const TargetLibraryInfo &TLI);

private:
  // Internal Instruction wrapper that contains more information about the
  // Instruction from prior analysis.
  struct InstructionInfo {
    // Instrumentation emitted for this instruction is for a compounded set of
    // read and write operations in the same basic block.
    static constexpr unsigned kCompoundRW = (1U << 0);

    explicit InstructionInfo(Instruction *Inst) : Inst(Inst) {}

    Instruction *Inst;
    unsigned Flags = 0;
  };

  void initialize(Module &M, const TargetLibraryInfo &TLI);
  bool instrumentLoadOrStore(const InstructionInfo &II, const DataLayout &DL);
  bool instrumentAtomic(Instruction *I, const DataLayout &DL);
  bool instrumentMemIntrinsic(Instruction *I);
  void chooseInstructionsToInstrument(SmallVectorImpl<Instruction *> &Local,
                                      SmallVectorImpl<InstructionInfo> &All,
                                      const DataLayout &DL);
  bool addrPointsToConstantData(Value *Addr);
  int getMemoryAccessFuncIndex(Type *OrigTy, Value *Addr, const DataLayout &DL);
  void InsertRuntimeIgnores(Function &F);
  void InsertAtomicEventSend(IRBuilder<> &IRB, uint8_t Eid, Value *Addr);
  void InsertAtomicEventSend(IRBuilder<> &IRB, uint8_t Eid, Value *Addr, Value *Val);
  void InsertAtomicEventLock(IRBuilder<> &IRB, Value *Addr);
  void InsertAtomicEventUnlock(IRBuilder<> &IRB, Value *Addr);
  void InsertEventSend(IRBuilder<> &IRB, uint8_t Eid);
  void InsertEventSend(IRBuilder<> &IRB, uint8_t Eid, Value *Addr);
  void InsertEventSend(IRBuilder<> &IRB, uint8_t Eid, Value *Addr, Value *Val);
  void InsertEventSend(IRBuilder<> &IRB, Value *Event);
  Value* FetchAndUpdateCounter(IRBuilder<> &IRB, Value *Addr);
  void PromoteIdxLoadStores(Function &F);
  GlobalVariable *TsanChannelPtr;
  GlobalVariable *TsanChannelIdx;
  GlobalVariable *TsanCounters;
  GlobalVariable *TsanSampling;
  Value *Channel;
  Value *Counters;
  Value *Sampling;
#if MONITOR_USE_LOCAL_IDX
  AllocaInst *LocalIdx;
#endif
  uint64_t TotalIdxsPromoted;

#if MONITOR_DEBUG
  // For debugging
  GlobalVariable *EventFormatString;
#endif

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

PreservedAnalyses ThreadSanitizerPass::run(Function &F,
                                           FunctionAnalysisManager &FAM) {
  ThreadSanitizer TSan;
  if (TSan.sanitizeFunction(F, FAM, FAM.getResult<TargetLibraryAnalysis>(F)))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

PreservedAnalyses ModuleThreadSanitizerPass::run(Module &M,
                                                 ModuleAnalysisManager &MAM) {
  insertModuleCtor(M);
  return PreservedAnalyses::none();
}
void ThreadSanitizer::initialize(Module &M, const TargetLibraryInfo &TLI) {
  const DataLayout &DL = M.getDataLayout();
  LLVMContext &Ctx = M.getContext();
  IntptrTy = DL.getIntPtrType(Ctx);

  IRBuilder<> IRB(Ctx);
  auto *ChannelPtr = M.getOrInsertGlobal("__tsan_channel_ptr", IRB.getPtrTy(), [&] {
    auto *GV = new GlobalVariable(M, IRB.getPtrTy(), /*isConstant=*/false,
                                  GlobalValue::ExternalLinkage, nullptr,
                                  "__tsan_channel_ptr", nullptr,
                                  GlobalVariable::InitialExecTLSModel);
    appendToCompilerUsed(M, GV);
    return GV;
  });
  TsanChannelPtr = cast<GlobalVariable>(ChannelPtr);
  auto *ChannelIdx = M.getOrInsertGlobal("__tsan_channel_idx", IRB.getInt32Ty(), [&] {
    auto *GV = new GlobalVariable(M, IRB.getInt32Ty(), /*isConstant=*/false,
                                  GlobalValue::ExternalLinkage, nullptr,
                                  "__tsan_channel_idx", nullptr,
                                  GlobalVariable::InitialExecTLSModel);
    appendToCompilerUsed(M, GV);
    return GV;
  });
  TsanChannelIdx = cast<GlobalVariable>(ChannelIdx);
  auto *Counters = M.getOrInsertGlobal("__tsan_counters", IRB.getPtrTy(), [&] {
    auto *GV = new GlobalVariable(M, IRB.getPtrTy(), /*isConstant=*/false,
                                  GlobalValue::ExternalLinkage, nullptr,
                                  "__tsan_counters", nullptr);
    appendToCompilerUsed(M, GV);
    return GV;
  });
  TsanCounters = cast<GlobalVariable>(Counters);
#if MONITOR_SAMPLING
  auto *Sampling = M.getOrInsertGlobal("__tsan_sampling", IRB.getInt8Ty(), [&] {
    auto *GV = new GlobalVariable(M, IRB.getInt8Ty(), /*isConstant=*/false,
                                  GlobalValue::ExternalLinkage, nullptr,
                                  "__tsan_sampling", nullptr,
                                  GlobalVariable::InitialExecTLSModel);
    appendToCompilerUsed(M, GV);
    return GV;
  });
  TsanSampling = cast<GlobalVariable>(Sampling);
#endif
#if MONITOR_DEBUG
  // Create a global string for our printf format
  Constant *EventFormatStrC = ConstantDataArray::getString(
    M.getContext(),
    "Event: %p %x\n"
  );
  EventFormatString = new GlobalVariable(
    M,
    EventFormatStrC->getType(),
    true,
    GlobalValue::PrivateLinkage,
    EventFormatStrC,
    "event format string"
  );
#endif

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
    SmallVectorImpl<InstructionInfo> &All, const DataLayout &DL) {
  DenseMap<Value *, size_t> WriteTargets; // Map of addresses to index in All
  // Iterate from the end.
  for (Instruction *I : reverse(Local)) {
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

    if (isa<AllocaInst>(getUnderlyingObject(Addr)) &&
        !PointerMayBeCaptured(Addr, true, true)) {
      // The variable is addressable but not captured, so it cannot be
      // referenced from a different thread and participate in a data race
      // (see llvm/Analysis/CaptureTracking.h for details).
      NumOmittedNonCaptured++;
      continue;
    }

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

static bool isTsanAtomic(const Instruction *I) {
  // TODO: Ask TTI whether synchronization scope is between threads.
  auto SSID = getAtomicSyncScopeID(I);
  if (!SSID)
    return false;
  if (isa<LoadInst>(I) || isa<StoreInst>(I))
    return *SSID != SyncScope::SingleThread;
  return true;
}

void ThreadSanitizer::InsertRuntimeIgnores(Function &F) {
  InstrumentationIRBuilder IRB(F.getEntryBlock().getFirstNonPHI());
#if MONITOR_CALL_HANDLERS
  IRB.CreateCall(TsanIgnoreBegin);
#endif
#if MONITOR_CALL_LOGGER
  InsertEventSend(IRB, 0xff);
#endif
  EscapeEnumerator EE(F, "tsan_ignore_cleanup", ClHandleCxxExceptions);
  while (IRBuilder<> *AtExit = EE.Next()) {
    InstrumentationIRBuilder::ensureDebugInfo(*AtExit, F);
#if MONITOR_CALL_HANDLERS
    AtExit->CreateCall(TsanIgnoreEnd);
#endif
#if MONITOR_CALL_LOGGER
    InsertEventSend(*AtExit, 0xff);
#endif
  }
}

void ThreadSanitizer::InsertAtomicEventLock(IRBuilder<> &IRB, Value *Addr) {
  auto *Cast = IRB.CreateCast(Instruction::PtrToInt, Addr, IRB.getInt64Ty());
  auto *Trunc = IRB.CreateAnd(Cast, IRB.getInt64(0xfffff));
  auto *Ptr = IRB.CreateGEP(
    IRB.getInt32Ty(),                         // The type of elements in the array
    Counters,                                  // The pointer to the start of the array
    IRB.CreateAdd(IRB.CreateShl(Trunc, IRB.getInt64(1)), IRB.getInt64(1))     // The index to access
  );

  auto BeforeCmpXchg = IRB.GetInsertPoint();
  // dwslim: Need to confirm if the ordering is ok.
  // cmp 0 - check if it is unlocked
  // new 1 - set to lock
  auto *Result = IRB.CreateAtomicCmpXchg(Ptr, IRB.getInt32(0), IRB.getInt32(1), MaybeAlign(),
                                         AtomicOrdering::AcquireRelease, AtomicOrdering::Acquire);
  auto *Success = IRB.CreateExtractValue(Result, /*Idxs=*/1);
  // TODO(dwslim): LLVM generates `setne al; test al, 0x1` but I think we actually only need 1 instruction
  auto *Fail = IRB.CreateICmpEQ(Success, IRB.getInt8(0));
  // It may look stupid that I'm doing this, but otherwise LLVM will do some even more stupid (or am I stupid?).
  // If I just pass `Success` directly and call SplitBlockAndInsertIfElse,
  // it generates jne; jmp instead of just one je.
  auto *LoopBB = SplitBlock(IRB.GetInsertBlock(), BeforeCmpXchg);
  auto ContPoint = IRB.GetInsertPoint();
  SplitBlockAndInsertIfThen(Fail, IRB.GetInsertPoint(), false,
                            nullptr, nullptr, nullptr,  // not sure what branch weights are good
                            LoopBB);
  IRB.SetInsertPoint(ContPoint);
}

void ThreadSanitizer::InsertAtomicEventUnlock(IRBuilder<> &IRB, Value *Addr) {
  auto *Cast = IRB.CreateCast(Instruction::PtrToInt, Addr, IRB.getInt64Ty());
  auto *Trunc = IRB.CreateAnd(Cast, IRB.getInt64(0xfffff));
  auto *Ptr = IRB.CreateGEP(
    IRB.getInt32Ty(),                         // The type of elements in the array
    Counters,                                  // The pointer to the start of the array
    IRB.CreateAdd(IRB.CreateShl(Trunc, IRB.getInt64(1)), IRB.getInt64(1))     // The index to access
  );

  // dwslim: Need to confirm if the ordering is ok.
  // cmp 0 - check if it is unlocked
  // new 1 - set to lock
  auto *Store = IRB.CreateStore(IRB.getInt32(0), Ptr);
  Store->setAtomic(AtomicOrdering::Release);
}

Value* ThreadSanitizer::FetchAndUpdateCounter(IRBuilder<> &IRB, Value *Addr) {
  // Perform the GEP to get the element pointer: Channel[Idx]
  // auto *Trunc = IRB.CreateAnd(IRB.CreateLShr(Addr, 4), IRB.getInt32(0xfffff));
  auto *Cast = IRB.CreateCast(Instruction::PtrToInt, Addr, IRB.getInt64Ty());
  auto *Trunc = IRB.CreateAnd(Cast, IRB.getInt64(0xfffff));
  auto *Ptr = IRB.CreateGEP(
    IRB.getInt32Ty(),                         // The type of elements in the array
    Counters,                                  // The pointer to the start of the array
    IRB.CreateAdd(IRB.CreateShl(Trunc, IRB.getInt64(1)), IRB.getInt64(0))     // The index to access
  );

  auto *Count = IRB.CreateAtomicRMW(AtomicRMWInst::Add, Ptr, IRB.getInt32(1), MaybeAlign(), AtomicOrdering::Monotonic);
  return Count;
}

void ThreadSanitizer::InsertAtomicEventSend(IRBuilder<> &IRB, uint8_t Eid, Value *Addr) {
  auto *Id = IRB.CreateShl(IRB.getInt64(Eid), 56);
  auto *Event = IRB.CreateOr(Id, Addr);
  auto *Count = FetchAndUpdateCounter(IRB, Addr);
  InsertEventSend(IRB, Event);
  InsertEventSend(IRB, IRB.CreateCast(Instruction::ZExt, IRB.CreateAdd(IRB.getInt32(0xcafe0000), Count), IRB.getInt64Ty()));
}

void ThreadSanitizer::InsertAtomicEventSend(IRBuilder<> &IRB, uint8_t Eid, Value *Addr, Value *Val) {
  auto *Id = IRB.CreateShl(IRB.getInt64(Eid), 56);
  auto *Event = IRB.CreateOr(Id, Addr);
  auto *Count = FetchAndUpdateCounter(IRB, Addr);
  InsertEventSend(IRB, Event);
  InsertEventSend(IRB, Val);
  InsertEventSend(IRB, Count);
}

void ThreadSanitizer::InsertEventSend(IRBuilder<> &IRB, uint8_t Eid) {
  auto *Event = IRB.CreateShl(IRB.getInt64(Eid), 56);
  InsertEventSend(IRB, Event);
}

void ThreadSanitizer::InsertEventSend(IRBuilder<> &IRB, uint8_t Eid, Value *Addr) {
  auto *Id = IRB.CreateShl(IRB.getInt64(Eid), 56);
  auto *Event = IRB.CreateOr(Id, Addr);
  InsertEventSend(IRB, Event);
}

void ThreadSanitizer::InsertEventSend(IRBuilder<> &IRB, uint8_t Eid, Value *Addr, Value *Val) {
  auto *Id = IRB.CreateShl(IRB.getInt64(Eid), 56);
  auto *Event = IRB.CreateOr(Id, Addr);
  InsertEventSend(IRB, Event);
  InsertEventSend(IRB, Val);
}

void ThreadSanitizer::InsertEventSend(IRBuilder<> &IRB, Value *Event) {
  // Check if sampling
#if MONITOR_SAMPLING
  auto *IsSampling = IRB.CreateICmpNE(Sampling, IRB.getInt8(0));
#endif

  // // Split the basic block for conditionally logging. Taken from BoundsChecking.cpp
  // BasicBlock::iterator SplitI = IRB.GetInsertPoint();
  // BasicBlock *OldBB = SplitI->getParent();
  // BasicBlock *Cont = OldBB->splitBasicBlock(SplitI);

  // // Create the basic block that contains the logging instructions.
  // Function *Fn = IRB.GetInsertBlock()->getParent();
  // LLVMContext& Context = Fn->getContext();
  // auto *LogBB = BasicBlock::Create(Context, "log", Fn, Cont);

  // // Rewire the old BB
  // OldBB->getTerminator()->eraseFromParent();
  // IRB.CreateCondBr(IsSampling, LogBB, Cont);

  // Split the basic block for conditionally logging. Taken from AddressSanitizer.cpp
#if MONITOR_SAMPLING
  auto ContPoint = IRB.GetInsertPoint();
  auto *LogBB =
    SplitBlockAndInsertIfThen(IsSampling, IRB.GetInsertPoint(), false,
                              MDBuilder(IRB.getContext()).createUnlikelyBranchWeights());
  IRB.SetInsertPoint(LogBB);
#endif

  {
    // Insert instructions for logging as per normal
    // if (!Channel)
    //   Channel = IRB.CreateLoad(TsanChannelPtr->getValueType(), TsanChannelPtr);
#if MONITOR_USE_LOCAL_IDX
    auto *Idx = IRB.CreateLoad(LocalIdx->getAllocatedType(), LocalIdx);
#else
    auto *Idx = IRB.CreateLoad(TsanChannelIdx->getValueType(), TsanChannelIdx);
#endif
    auto *Trunc = IRB.CreateAnd(Idx, IRB.getInt32(0xff));     // cannot use CreateTrunc because that performs sign extend
    // Perform the GEP to get the element pointer: Channel[Idx]
    auto *Ptr = IRB.CreateGEP(
      IRB.getInt64Ty(),           // The type of elements in the array
      Channel,                    // The pointer to the start of the array
      Trunc                        // The index to access
    );
    // auto *Ptr = IRB.CreateGEP(
    //   IRB.getInt64Ty(),           // The type of elements in the array
    //   Channel,                    // The pointer to the start of the array
    //   IRB.getInt16(0)                        // The index to access
    // );
    auto *Inc = IRB.CreateAdd(Idx, IRB.getInt32(1));
    // IRB.CreateStore(Idx, Ptr);
    IRB.CreateStore(Event, Ptr);
#if MONITOR_USE_LOCAL_IDX
    IRB.CreateStore(Inc, LocalIdx);
#else
    IRB.CreateStore(Inc, TsanChannelIdx);
#endif

#if MONITOR_DEBUG
    // Declare printf if it's not already declared
    Module* M = IRB.GetInsertBlock()->getParent()->getParent();
    FunctionCallee Printf = M->getOrInsertFunction(
      "printf",
      FunctionType::get(
        IntegerType::getInt32Ty(M->getContext()),
        PointerType::get(Type::getInt8Ty(M->getContext()), 0),
        true /* this is vararg */
      )
    );

    // Create printf arguments
    Value *Args[] = {
      EventFormatString,
      Channel,
      Idx
    };

    // Create the printf call
    IRB.CreateCall(Printf, Args);
#endif
  }

#if MONITOR_SAMPLING
  IRB.SetInsertPoint(ContPoint);
#endif
}

bool ThreadSanitizer::sanitizeFunction(Function &F,
                                       FunctionAnalysisManager &FAM,
                                       const TargetLibraryInfo &TLI) {
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

  initialize(*F.getParent(), TLI);
  SmallVector<InstructionInfo, 8> AllLoadsAndStores;
  SmallVector<Instruction*, 8> LocalLoadsAndStores;
  SmallVector<Instruction*, 8> AtomicAccesses;
  SmallVector<Instruction*, 8> MemIntrinCalls;
  SmallVector<Instruction*, 8> Calls;
  bool Res = false;
  bool HasCalls = false;
  bool SanitizeFunction = F.hasFnAttribute(Attribute::SanitizeThread);
  const DataLayout &DL = F.getParent()->getDataLayout();

  // Traverse all instructions, collect loads/stores/returns, check for calls.
  for (auto &BB : F) {
    for (auto &Inst : BB) {
      // Skip instructions inserted by another instrumentation.
      if (Inst.hasMetadata(LLVMContext::MD_nosanitize))
        continue;
      if (isTsanAtomic(&Inst))
        AtomicAccesses.push_back(&Inst);
      else if (isa<LoadInst>(Inst) || isa<StoreInst>(Inst))
        LocalLoadsAndStores.push_back(&Inst);
      else if ((isa<CallInst>(Inst) && !isa<DbgInfoIntrinsic>(Inst)) ||
               isa<InvokeInst>(Inst)) {
        if (CallInst *CI = dyn_cast<CallInst>(&Inst))
          maybeMarkSanitizerLibraryCallNoBuiltin(CI, &TLI);
        if (isa<MemIntrinsic>(Inst))
          MemIntrinCalls.push_back(&Inst);
        // dwslim: Be conservative for now and record all calls.
        Calls.push_back(&Inst);
        HasCalls = true;
        chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores,
                                       DL);
      }
    }
    chooseInstructionsToInstrument(LocalLoadsAndStores, AllLoadsAndStores, DL);
  }

  // We have collected all loads and stores.
  // FIXME: many of these accesses do not need to be checked for races
  // (e.g. variables that do not escape, etc).

  // Load the necessary values before doing instrumentation.
  InstrumentationIRBuilder IRB(F.getEntryBlock().getFirstNonPHI());
  Channel = IRB.CreateLoad(TsanChannelPtr->getValueType(), TsanChannelPtr);
  auto *Idx = IRB.CreateLoad(TsanChannelIdx->getValueType(), TsanChannelIdx);
  Counters = IRB.CreateLoad(TsanCounters->getValueType(), TsanCounters);
#if MONITOR_USE_LOCAL_IDX
  LocalIdx = IRB.CreateAlloca(Idx->getType());
  IRB.CreateStore(Idx, LocalIdx);
#endif

#if MONITOR_SAMPLING
  Sampling = IRB.CreateLoad(TsanSampling->getValueType(), TsanSampling);
#endif

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

  if (ClInstrumentMemIntrinsics && SanitizeFunction)
    for (auto *Inst : MemIntrinCalls) {
      Res |= instrumentMemIntrinsic(Inst);
    }

  if (F.hasFnAttribute("sanitize_thread_no_checking_at_run_time")) {
    assert(!F.hasFnAttribute(Attribute::SanitizeThread));
    if (HasCalls)
      InsertRuntimeIgnores(F);
  }

#if MONITOR_USE_LOCAL_IDX && MONITOR_SAVE_INFO_FOR_CALLS
  // Before each function call, we need to update the ChannelIdx global variable.
  // After each function call we also need to load from it.
  // If did not instrument any accesses, then LocalIdx is not used.
  if (Res) {
    for (const auto &CI : Calls) {
      InstrumentationIRBuilder IRB(CI);
      // Save the local index
      auto *Load1 = IRB.CreateLoad(LocalIdx->getAllocatedType(), LocalIdx);
      IRB.CreateStore(Load1, TsanChannelIdx);

      // No need to restore local index for tail calls since the function is gonna return already
      if (isa<CallInst>(CI) && dyn_cast<CallInst>(CI)->isTailCall())
        continue;
      if (isa<InvokeInst>(CI) && dyn_cast<InvokeInst>(CI)->isTailCall())
        continue;

      auto Next = std::next(CI->getIterator());
      // if (Next == CI->getParent()->end()) {
      //   if (!CI->isTerminator())
      //     IRB.SetInsertPoint(CI->getParent());
      //   // TODO(dwslim): fix this! probably just insert to the next basic block
      //   // but if it is a terminator, shouldn't it have been a tail call?
      //   // IRB.SetInsertPoint(CI->getParent());
      //   else
      //     continue;
      // }
      // else
      //   IRB.SetInsertPoint(CI->getParent(), Next);

      // Update the local index
      IRB.SetInsertPoint(Next);
      auto *Load2 = IRB.CreateLoad(TsanChannelIdx->getValueType(), TsanChannelIdx);
      IRB.CreateStore(Load2, LocalIdx);
    }

    EscapeEnumerator EE(F, "tsan_cleanup", ClHandleCxxExceptions);
    while (IRBuilder<> *AtExit = EE.Next()) {
      InstrumentationIRBuilder::ensureDebugInfo(*AtExit, F);
      // AtExit->CreateCall(TsanFuncExit, {});
      // dwslim: Store the global idx before exiting the function
      auto *Load = AtExit->CreateLoad(LocalIdx->getAllocatedType(), LocalIdx);
      AtExit->CreateStore(Load, TsanChannelIdx);
    }
  }
#endif

  // Instrument function entry/exit points if there were instrumented accesses.
  if ((Res || HasCalls) && ClInstrumentFuncEntryExit) {
    // // Monitor: Bye bye
    // InstrumentationIRBuilder IRB(F.getEntryBlock().getFirstNonPHI());
    // Reuse IRB from earlier, so that the event sending code comes after loading
    // the channel pointer and index.
    Value *ReturnAddress = IRB.CreateCall(
        Intrinsic::getDeclaration(F.getParent(), Intrinsic::returnaddress),
        IRB.getInt32(0));
    // IRB.CreateCall(TsanFuncEntry, ReturnAddress);
    InsertEventSend(IRB, 0, ReturnAddress);

    // // Monitor: Bye bye
    EscapeEnumerator EE(F, "tsan_cleanup", ClHandleCxxExceptions);
    while (IRBuilder<> *AtExit = EE.Next()) {
      InstrumentationIRBuilder::ensureDebugInfo(*AtExit, F);
      // AtExit->CreateCall(TsanFuncExit, {});
      InsertEventSend(*AtExit, 1);
    }
    Res = true;
  }

#if MONITOR_USE_LOCAL_IDX
  // dwslim: I had to put this check because in some cases it is not promotable.
  // We need to find out if this is acceptable or how to circumvent this.
  // errs() << "Is alloca promotable: " << isAllocaPromotable(LocalIdx) << "\n";
  if (isAllocaPromotable(LocalIdx)) {
    SmallVector<AllocaInst*, 1> Allocas;
    Allocas.push_back(LocalIdx);
    auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
    auto &AC = FAM.getResult<AssumptionAnalysis>(F);
    PromoteMemToReg(Allocas, DT, &AC);
  }
#endif

  return Res;
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
#if MONITOR_CALL_HANDLERS
    IRB.CreateCall(TsanVptrUpdate, {Addr, StoredValue});
#endif
#if MONITOR_CALL_LOGGER
    InsertEventSend(IRB, 2, Addr, StoredValue);
#endif
    NumInstrumentedVtableWrites++;
    return true;
  }
  if (!IsWrite && isVtableAccess(II.Inst)) {
#if MONITOR_CALL_HANDLERS
    IRB.CreateCall(TsanVptrLoad, Addr);
#endif
#if MONITOR_CALL_LOGGER
    InsertEventSend(IRB, 3, Addr);
#endif
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
#if MONITOR_CALL_HANDLERS
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
  IRB.CreateCall(OnAccessFunc, Addr);
#endif
#if MONITOR_CALL_LOGGER
  InsertEventSend(IRB, 4, Addr);
#endif
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

// If a memset intrinsic gets inlined by the code gen, we will miss races on it.
// So, we either need to ensure the intrinsic is not inlined, or instrument it.
// We do not instrument memset/memmove/memcpy intrinsics (too complicated),
// instead we simply replace them with regular function calls, which are then
// intercepted by the run-time.
// Since tsan is running after everyone else, the calls should not be
// replaced back with intrinsics. If that becomes wrong at some point,
// we will need to call e.g. __tsan_memset to avoid the intrinsics.
bool ThreadSanitizer::instrumentMemIntrinsic(Instruction *I) {
  InstrumentationIRBuilder IRB(I);
  if (MemSetInst *M = dyn_cast<MemSetInst>(I)) {
#if MONITOR_CALL_HANDLERS
    Value *Cast1 = IRB.CreateIntCast(M->getArgOperand(1), IRB.getInt32Ty(), false);
    Value *Cast2 = IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false);
    IRB.CreateCall(
        MemsetFn,
        {M->getArgOperand(0),
         Cast1,
         Cast2});
    I->eraseFromParent();
#endif
#if MONITOR_CALL_LOGGER
  InsertEventSend(IRB, 5);
#endif
  } else if (MemTransferInst *M = dyn_cast<MemTransferInst>(I)) {
#if MONITOR_CALL_HANDLERS
    IRB.CreateCall(
        isa<MemCpyInst>(M) ? MemcpyFn : MemmoveFn,
        {M->getArgOperand(0),
         M->getArgOperand(1),
         IRB.CreateIntCast(M->getArgOperand(2), IntptrTy, false)});
    I->eraseFromParent();
#endif
#if MONITOR_CALL_LOGGER
  InsertEventSend(IRB, 6);
#endif
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
#if MONITOR_CALL_ATOMIC_HANDLERS
    int Idx = getMemoryAccessFuncIndex(OrigTy, Addr, DL);
    if (Idx < 0)
      return false;
    Value *Args[] = {Addr,
                     createOrdering(&IRB, LI->getOrdering())};
    Value *C = IRB.CreateCall(TsanAtomicLoad[Idx], Args);
    Value *Cast = IRB.CreateBitOrPointerCast(C, OrigTy);
    I->replaceAllUsesWith(Cast);
#endif
#if MONITOR_CALL_LOGGER
  // do this after the load
  IRB.SetInsertPoint(I->getNextNode());
  InsertAtomicEventSend(IRB, 7, Addr);
#endif
  } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    Value *Addr = SI->getPointerOperand();
#if MONITOR_CALL_ATOMIC_HANDLERS
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
#endif
#if MONITOR_CALL_LOGGER
  InsertAtomicEventSend(IRB, 8, Addr);
#endif
  } else if (AtomicRMWInst *RMWI = dyn_cast<AtomicRMWInst>(I)) {
    Value *Addr = RMWI->getPointerOperand();
#if MONITOR_CALL_ATOMIC_HANDLERS
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
#endif
#if MONITOR_CALL_LOGGER
  InsertAtomicEventLock(IRB, Addr);
  InsertAtomicEventSend(IRB, 9, Addr);
  IRB.SetInsertPoint(RMWI->getNextNode());
  InsertAtomicEventUnlock(IRB, Addr);
#endif
  } else if (AtomicCmpXchgInst *CASI = dyn_cast<AtomicCmpXchgInst>(I)) {
    Value *Addr = CASI->getPointerOperand();
    Type *OrigOldValTy = CASI->getNewValOperand()->getType();
#if MONITOR_CALL_ATOMIC_HANDLERS
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
#endif
#if MONITOR_CALL_LOGGER
  InsertAtomicEventLock(IRB, Addr);
  InsertAtomicEventSend(IRB, 10, Addr);
  IRB.SetInsertPoint(CASI->getNextNode());
  InsertAtomicEventUnlock(IRB, Addr);
#endif
  } else if (FenceInst *FI = dyn_cast<FenceInst>(I)) {
#if MONITOR_CALL_ATOMIC_HANDLERS
    Value *Args[] = {createOrdering(&IRB, FI->getOrdering())};
    FunctionCallee F = FI->getSyncScopeID() == SyncScope::SingleThread
                           ? TsanAtomicSignalFence
                           : TsanAtomicThreadFence;
    IRB.CreateCall(F, Args);
    FI->eraseFromParent();
#endif
#if MONITOR_CALL_LOGGER
  // dwslim: tsan doesnt handle fences, i also dont really know what to do with this yet
  // lets see if we even ever need to handle this
  InsertEventSend(IRB, 11);
#endif
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
