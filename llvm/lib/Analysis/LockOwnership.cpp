//==- LockOwnershipAnalysis.cpp --==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the generic Escape Analysis interface.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/LockOwnership.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/CFG.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

#include <fstream>

using namespace llvm;

#define DEBUG_TYPE "lock-ownership"

SmallVector<std::vector<CallGraphNode *>>
LockOwnershipInfo::getTopDownSCCList(CallGraph &CG) {
  SmallVector<std::vector<CallGraphNode *>> SCCList;
  for (auto It = scc_begin(&CG); !It.isAtEnd(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;
    assert(!SCC.empty() && "SCC with no functions?");
    SCCList.push_back(SCC);
  }
  return SCCList;
}

LockStateTy
LockOwnershipInfo::intersectLockStates(LockStateTy MeetState,
                                       const LockStateTy &PredOutState) {
  LockStateTy NextMeetState;
  for (auto const &[Lock, State] : MeetState) {
    // Check if this mutex exists and has been also locked in predOutState
    const auto PredIt = PredOutState.find(Lock);
    if (PredIt != PredOutState.end() &&
        PredIt->second.IsLocked == State.IsLocked) {
      // Only keep if the state is identical in both
      if (PredIt->second.LockInstr == State.LockInstr)
        NextMeetState[Lock] = {State.IsLocked, State.LockInstr};
      else {
        // This unlock point is no more unambiguous now because of merge
        // of control flows
        LLVM_DEBUG(dbgs() << "WARNING: Lock point is unambiguous: " << *Lock
                          << "\n");
        LLVM_DEBUG({
          dbgs() << "  Pred lock instr: ";
          if (PredIt->second.LockInstr)
            dbgs() << *PredIt->second.LockInstr;
          dbgs() << "\n  Current lock instr: ";
          if (State.LockInstr)
            dbgs() << *State.LockInstr;
          dbgs() << "\n";
        });

        // NextMeetState[Lock] = {State.IsLocked, nullptr};
        // NextMeetState.erase(Lock);
        NextMeetState[Lock] = {State.IsLocked, PredIt->second.LockInstr};
      }
    }
    // Otherwise, the state diverges, so we don't include it in the 'must be
    // locked' state
  }
  return NextMeetState;
}

/// Meet Operator: Intersects the lock states from predecessors
LockStateTy LockOwnershipInfo::computeMeet(const BasicBlock *BB,
                                           BBStateMap &OutStates) {
  LockStateTy MeetState;
  bool FirstPred = true;

  for (const BasicBlock *Pred : predecessors(BB)) {
    // Find the out-state of the predecessor
    const auto It = OutStates.find(Pred);
    if (It == OutStates.end()) {
      // Predecessor state not computed yet (should not happen with a proper
      // worklist init) Treat as if nothing is locked coming from this path
      continue; // Or return an empty map / handle error
    }
    const LockStateTy &PredOutState = It->second;

    if (FirstPred) {
      MeetState = PredOutState; // Initialize with the first predecessor's state
      FirstPred = false;
    } else {
      // Intersect MeetState with PredOutState
      MeetState = intersectLockStates(MeetState, PredOutState);
    }
  }

  // If a block has no predecessors (entry block), the meet result is an empty
  // map (initial state)
  if (pred_empty(BB))
    return LockStateTy(); // Initial state: nothing locked

  return MeetState;
}

/// The identity of the mutex a lock call names, or null if it cannot be
/// known.
///
/// A mutex is a specific object, not the allocation it sits in. Identifying a
/// lock by getUnderlyingObject() collapsed every mutex reachable from one base
/// into a single lock -- the two fields of one struct, the elements of a
/// striped array -- and accepted a pointer argument or a loaded pointer as an
/// identity although it names a different mutex on every call. Each of those
/// let the analysis conclude that two accesses were under the same lock when
/// they were not, and drop the instrumentation that would have caught the
/// race (tsan-experiments/tools/tmp-checks/lo-soundness: three lost races).
///
/// So a lock is known only when it is a global, or a constant offset into a
/// global. That is canonicalised to (global, byte offset) and rebuilt as a
/// uniqued constant GEP, so the same mutex reached through different
/// instructions in different functions compares equal. Anything else -- an
/// argument, a load, a variable-index GEP, a local -- is unknown, and an
/// unknown mutex protects nothing. That forgoes protection through a mutex
/// handed in by pointer even when every caller passes the same global; that is
/// the cost of not guessing.
static const Value *canonicalLockIdentity(const Value *Ptr,
                                          const DataLayout &DL) {
  // Lock functions are recognised by name, and some of what that matches
  // takes an integer first argument (clock_gettime's clockid_t,
  // __tsan_java_mutex_lock's jptr). An integer names no mutex we can track.
  if (!Ptr->getType()->isPointerTy())
    return nullptr;
  APInt Offset(DL.getIndexTypeSizeInBits(Ptr->getType()), 0);
  const Value *Base = Ptr->stripAndAccumulateConstantOffsets(
      DL, Offset, /*AllowNonInbounds=*/true);
  const auto *GV = dyn_cast<GlobalVariable>(Base);
  if (!GV)
    return nullptr;
  if (Offset.isZero())
    return GV;
  LLVMContext &Ctx = GV->getContext();
  Value *Idx = ConstantInt::get(Ctx, Offset);
  return ConstantExpr::getGetElementPtr(Type::getInt8Ty(Ctx),
                                        const_cast<GlobalVariable *>(GV), Idx);
}

std::pair<LockOwnershipInfo::LockCallType, const Value *>
LockOwnershipInfo::getLockCallInfo(const Instruction *I) const {
  if (!I)
    return {LockCallType::NONE, nullptr};

  if (const auto *CB = dyn_cast<CallBase>(I)) {
    Function *CalledFunc = CB->getCalledFunction();

    if (!CalledFunc || !CalledFunc->isDeclaration() ||
        (CalledFunc->arg_size() == 0))
      return {LockCallType::NONE, nullptr};

    // A shared acquisition provides no exclusion, so it is not recorded as
    // holding anything. A global reached only under reader locks therefore
    // ends up with an empty lockset and stays instrumented.
    // The Value is null when the mutex cannot be identified; the caller
    // decides what an unknown acquisition or release means.
    if (isLockFunc(CalledFunc) && !isSharedLockFunc(CalledFunc) &&
        !isTryLockFunc(CalledFunc))
      return {LockCallType::LOCK, canonicalLockIdentity(CB->getArgOperand(0),
                                                        M.getDataLayout())};
    if (isUnLockFunc(CalledFunc))
      return {LockCallType::UNLOCK, canonicalLockIdentity(
                                        CB->getArgOperand(0), M.getDataLayout())};
  }
  return {LockCallType::NONE, nullptr};
}

void LockOwnershipInfo::handleLock(LockStateTy &CurrState,
                                   const Instruction &Instr,
                                   const Value *Lock) {
  LLVM_DEBUG(dbgs() << "\nLOCK Instr: " << Instr << "\n");
  const auto It = CurrState.find(Lock);
  if (It != CurrState.end() && It->second.IsLocked && It->second.LockInstr) {
    // Already locked! Potential double lock.
    LLVM_DEBUG(dbgs() << "WARNING: Double lock on mutex: " << *Lock << "\n");
    // Keep tracking the *first* lock instruction for this path
  } else if (It != CurrState.end()) {
    // An entry without a live acquisition is stale; this is a fresh one.
    It->second = {true, &Instr};
  } else {
    // Not locked or not present, mark as locked
    LLVM_DEBUG(dbgs() << "Lock: " << *Lock << "\n");
    CurrState[Lock] = {true, &Instr};
  }
}

void LockOwnershipInfo::handleUnlock(LockStateTy &CurrState,
                                     const Instruction &Instr,
                                     const Value *Lock) {
  LLVM_DEBUG(dbgs() << "\nUNLOCK Instr: " << Instr << "\n");

  const auto It = CurrState.find(Lock);
  if (It != CurrState.end()) {
    // Was locked, now unlocked. Record the pair only when there is an
    // acquisition to pair it with; a release with none recorded (the lock was
    // taken by a caller, or the state was merged away) is not a region. This
    // used to assert, and memcached's extstore.c and MySQL's thr_mutex.cc
    // both reach it.
    LLVM_DEBUG(dbgs() << "Lock: " << *Lock << "\n");
    if (It->second.LockInstr) {
      LLVM_DEBUG(dbgs() << "Lock Instr: " << *It->second.LockInstr << "\n\n");
      LockUnlockPairs.insert({It->second.LockInstr, &Instr});
    }
    // Released either way.
    CurrState.erase(It);
  } else {
    // Unlocking a mutex that wasn't locked here: nothing was held, nothing to
    // record. (A marker entry used to be inserted; it only served to trip the
    // assertion above on the next acquisition.)
    LLVM_DEBUG(dbgs() << "WARNING: Unlocking a mutex that wasn't locked: "
                      << *Lock << "\n");
  }
}

/// Transfer Function: Applies block's instructions to the in-state
/// Returns true if the out-state *changes* as a result
bool LockOwnershipInfo::applyTransferFunc(const BasicBlock *BB,
                                          LockStateTy &InState,
                                          LockStateTy &OutState,
                                          bool InstrToLockFlag) {
  for (const Instruction &I : *BB) {
    // LLVM_DEBUG(dbgs() << "\n\tInstr: " << I << "\n");
    const auto [CallType, Lock] = getLockCallInfo(&I);

    // 1. Process lock acquire and release
    if (CallType == LockCallType::LOCK) {
      // Acquiring a mutex we cannot identify adds nothing we can rely on.
      if (Lock)
        handleLock(InState, I, Lock);
      continue;
    }

    if (CallType == LockCallType::UNLOCK) {
      if (Lock) {
        handleUnlock(InState, I, Lock);
      } else {
        // A release of a mutex we cannot identify may be the release of any
        // mutex we believe held -- a loaded or passed-in pointer can name the
        // same global. Nothing is known to be held after it. (A call into a
        // function without a body is still assumed not to release anything;
        // that is the pre-existing assumption for external calls and is left
        // as it was.)
        LLVM_DEBUG(dbgs() << "Unlock of unidentifiable mutex; clearing state: "
                          << I << "\n");
        InState.clear();
      }
      continue;
    }

    // 2. Process calls
    if (const auto *Call = dyn_cast<CallBase>(&I)) {
      const auto *CalledFunc = Call->getCalledFunction();
      if (CalledFunc && TransparentDecls.contains(CalledFunc))
        continue;
      if (!CalledFunc || CalledFunc->isDeclaration()) {
        // Indirect, or a body we cannot see: it may release any lock it can
        // reach, which is every non-private one plus whatever a callback
        // into this module releases.
        applyOpaqueCall(InState, I);
        continue;
      }
      // What the callee (transitively) releases takes effect before what it
      // still holds at its exit; a callee that unlocks and re-locks the same
      // mutex leaves it held.
      if (const auto RSIt = ReleaseSummaries.find(CalledFunc);
          RSIt != ReleaseSummaries.end())
        applyCalleeReleases(InState, I, RSIt->second);
      if (const auto FuncStateIt = FuncStates.find(CalledFunc);
          FuncStateIt != FuncStates.end())
        for (const auto &[Lock, State] : FuncStateIt->second.ExitState)
          if (State.IsLocked)
            handleLock(InState, I, Lock);
      continue;
    }

    // 3. Process other instructions - only if the flag is set
    if (InstrToLockFlag) {
      LLVM_DEBUG(dbgs() << "\tInstr to lock map: " << I << "\n");
      // Record all currently held locks for this instruction
      SmallPtrSet<const Value *, 4> HeldLocks;
      for (const auto &[Lock, State] : InState) {
        LLVM_DEBUG(dbgs() << "\t\tLock: " << *Lock << "\n");
        if (State.IsLocked)
          HeldLocks.insert(Lock);
      }
      // Replace whatever an earlier visit recorded. A block is visited again
      // when a predecessor's state shrinks, and its held set can shrink to
      // nothing; skipping the empty set left the earlier, larger record in
      // place and an unprotected access looked protected.
      if (HeldLocks.empty())
        InstrToLockMap.erase(&I);
      else
        InstrToLockMap[&I] = HeldLocks;
    }
  }

  // Check if the calculated out-state differs from the stored one
  if (OutState != InState) {
    OutState = InState; // Update the stored state
    return true;        // State changed
  }
  return false; // State did not change
}

/// Library functions that take a callback: the callee may call back into
/// this module and release a lock there, so they are not transparent.
static const StringSet<> CallbackLibFuncs = {
    "qsort",       "qsort_r",    "bsearch",       "atexit",   "__cxa_atexit",
    "on_exit",     "signal",     "sigaction",     "bsd_signal",
    "pthread_create", "pthread_once", "pthread_atfork", "pthread_key_create",
    "thrd_create", "call_once",  "tsearch",       "tfind",    "tdelete",
    "twalk",       "lfind",      "lsearch",       "ftw",      "nftw",
    "glob",        "scandir",    "dl_iterate_phdr"};

/// Mutex operations that do not acquire or release but are the only other
/// legitimate uses of a private mutex's address.
static const StringSet<> MutexAuxNames = {
    "pthread_mutex_init",    "pthread_mutex_destroy",  "pthread_spin_init",
    "pthread_spin_destroy",  "pthread_rwlock_init",    "pthread_rwlock_destroy",
    "pthread_cond_wait",     "pthread_cond_timedwait", "mtx_init",
    "mtx_destroy",           "cnd_wait",               "cnd_timedwait",
    "omp_init_lock",         "omp_destroy_lock",       "omp_init_nest_lock",
    "omp_destroy_nest_lock"};

/// Whether an opaque call could invoke \p F synchronously on the calling
/// thread. An externally visible function can be called from anywhere. A
/// local one only if its address is taken -- except as the start routine of
/// a thread creator: that runs on the new thread, which cannot release a
/// mutex the creating thread holds.
static bool mayBeCalledBack(const Function &F) {
  if (!TsanWholeProgram && !F.hasLocalLinkage())
    return true;
  for (const Use &U : F.uses()) {
    const auto *CB = dyn_cast<CallBase>(U.getUser());
    if (CB && CB->isCallee(&U))
      continue;
    if (CB && CB->isArgOperand(&U)) {
      const Function *Callee = CB->getCalledFunction();
      if (Callee && isKnownThreadCreator(*Callee))
        continue;
    }
    return true;
  }
  return false;
}

void LockOwnershipInfo::computeReleaseSummaries() {
  ReleaseSummaries.clear();
  TransparentDecls.clear();
  ReleasedByCallbacks.clear();
  CallbackMayReleaseUnknown = false;
  PrivateMutexCache.clear();

  TargetLibraryInfoImpl TLII(Triple(M.getTargetTriple()));
  TargetLibraryInfo TLI(TLII);
  for (const Function &F : M) {
    if (!F.isDeclaration())
      continue;
    // Lock and unlock calls are handled by the transfer function itself.
    if (F.isIntrinsic() || isAnnotationFunc(&F) || isLockFunc(&F) ||
        isUnLockFunc(&F)) {
      TransparentDecls.insert(&F);
      continue;
    }
    LibFunc LF;
    if (TLI.getLibFunc(F, LF) && TLI.has(LF) &&
        !CallbackLibFuncs.contains(F.getName()))
      TransparentDecls.insert(&F);
  }

  // 1. What each body does on its own, and whom it calls.
  DenseMap<const Function *, SmallPtrSet<const Function *, 8>> Callees;
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    ReleaseSummaryTy &RS = ReleaseSummaries[&F];
    for (const Instruction &I : instructions(F)) {
      const auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      const auto [CallType, Lock] = getLockCallInfo(&I);
      if (CallType == LockCallType::UNLOCK) {
        if (Lock)
          RS.MayRelease.insert(Lock);
        else
          RS.MayReleaseUnknown = true;
        continue;
      }
      if (CallType == LockCallType::LOCK)
        continue;
      const Function *Callee = CB->getCalledFunction();
      if (!Callee || (Callee->isDeclaration() &&
                      !TransparentDecls.contains(Callee))) {
        RS.CallsOpaque = true;
        continue;
      }
      if (!Callee->isDeclaration())
        Callees[&F].insert(Callee);
    }
  }

  // 2. Close over the call graph.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &[F, RS] : ReleaseSummaries) {
      for (const Function *C : Callees[F]) {
        const auto CIt = ReleaseSummaries.find(C);
        if (CIt == ReleaseSummaries.end())
          continue;
        const ReleaseSummaryTy &CS = CIt->second;
        const size_t Before = RS.MayRelease.size();
        RS.MayRelease.insert(CS.MayRelease.begin(), CS.MayRelease.end());
        const bool Unknown = RS.MayReleaseUnknown || CS.MayReleaseUnknown;
        const bool Opaque = RS.CallsOpaque || CS.CallsOpaque;
        if (RS.MayRelease.size() != Before || Unknown != RS.MayReleaseUnknown ||
            Opaque != RS.CallsOpaque) {
          RS.MayReleaseUnknown = Unknown;
          RS.CallsOpaque = Opaque;
          Changed = true;
        }
      }
    }
  }

  // 3. What an opaque call could release by calling back into this module.
  for (const auto &[F, RS] : ReleaseSummaries) {
    if (!mayBeCalledBack(*F))
      continue;
    ReleasedByCallbacks.insert(RS.MayRelease.begin(), RS.MayRelease.end());
    CallbackMayReleaseUnknown |= RS.MayReleaseUnknown;
  }
  LLVM_DEBUG({
    dbgs() << "Release summaries:\n";
    for (const auto &[F, RS] : ReleaseSummaries)
      dbgs() << "  " << F->getName() << ": " << RS.MayRelease.size()
             << " lock(s)" << (RS.MayReleaseUnknown ? ", unknown" : "")
             << (RS.CallsOpaque ? ", opaque" : "") << "\n";
    dbgs() << "  released by callbacks: " << ReleasedByCallbacks.size()
           << (CallbackMayReleaseUnknown ? " + unknown" : "") << "\n";
  });
}

bool LockOwnershipInfo::isPrivateMutex(const Value *Lock) const {
  const auto *GV = dyn_cast<GlobalVariable>(getUnderlyingObject(Lock));
  if (!GV || (!TsanWholeProgram && !GV->hasLocalLinkage()) ||
      GV->isDeclaration())
    return false;
  const auto [It, Inserted] = PrivateMutexCache.try_emplace(GV, false);
  if (!Inserted)
    return It->second;

  bool Private = true;
  SmallVector<const Value *, 8> Work{GV};
  SmallPtrSet<const Value *, 8> Seen;
  while (Private && !Work.empty()) {
    const Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    for (const User *U : V->users()) {
      if (isa<ConstantExpr>(U) || isa<GlobalAlias>(U)) {
        Work.push_back(U);
        continue;
      }
      const auto *CB = dyn_cast<CallBase>(U);
      const Function *Callee = CB ? CB->getCalledFunction() : nullptr;
      if (!Callee || CB->getCalledOperand() == V ||
          !(isLockFunc(Callee) || isUnLockFunc(Callee) ||
            MutexAuxNames.contains(Callee->getName()))) {
        Private = false;
        break;
      }
    }
  }
  PrivateMutexCache[GV] = Private;
  return Private;
}

void LockOwnershipInfo::applyOpaqueCall(LockStateTy &State,
                                        const Instruction &I) {
  SmallVector<const Value *, 4> Held;
  for (const auto &[Lock, LS] : State)
    if (LS.IsLocked)
      Held.push_back(Lock);
  for (const Value *Lock : Held)
    if (CallbackMayReleaseUnknown || ReleasedByCallbacks.contains(Lock) ||
        !isPrivateMutex(Lock))
      handleUnlock(State, I, Lock);
}

void LockOwnershipInfo::applyCalleeReleases(LockStateTy &State,
                                            const Instruction &I,
                                            const ReleaseSummaryTy &RS) {
  if (RS.MayReleaseUnknown) {
    State.clear();
    return;
  }
  for (const Value *Lock : RS.MayRelease)
    handleUnlock(State, I, Lock);
  if (RS.CallsOpaque)
    applyOpaqueCall(State, I);
}

void LockOwnershipInfo::buildSummary(const Function *F, bool InstrToLockFlag) {
  LLVM_DEBUG(dbgs() << "\n===============================================\n");
  LLVM_DEBUG(dbgs() << "Building summary for function: " << F->getName()
                    << "\n");

  auto [FuncStateIt, _] = FuncStates.try_emplace(F, FuncStateTy());
  BBStateMap &InStates = FuncStateIt->second.InStates;
  BBStateMap &OutStates = FuncStateIt->second.OutStates;

  // SmallPtrSet<const BasicBlock *, 8> Visited;
  const BasicBlock &EntryBB = F->getEntryBlock();

  std::deque<const BasicBlock *> WorkList;

  // Every block once, in reverse post-order, then change-driven. Seeding
  // with the entry alone never reached the blocks behind one whose state
  // did not change (the common case: no lock activity), and seeding in
  // layout order met a block before its forward predecessors and took an
  // optimistic intermediate state as the meet.
  for (const BasicBlock *BB : ReversePostOrderTraversal<const Function *>(F))
    WorkList.push_back(BB);

  while (!WorkList.empty()) {
    const BasicBlock *BB = WorkList.front();
    WorkList.pop_front();
    LLVM_DEBUG(dbgs() << "\nProcessing BB: " << BB->getName() << "\n");

    // 1. Compute IN state by meeting OUT states of predecessors
    // Skip for entry block as it's already initialized
    if (BB != &EntryBB)
      InStates[BB] = computeMeet(BB, OutStates);

    // 2. Apply transfer function to get OUT state
    LockStateTy CurrOutState = InStates[BB]; // Start with IN state
    // Modifies currentOutState in place
    const bool StateChanged =
        applyTransferFunc(BB, CurrOutState, OutStates[BB], InstrToLockFlag);

    // 3. If OUT state changed, add successors to the worklist
    if (StateChanged) {
      for (const BasicBlock *Succ : successors(BB))
        WorkList.push_back(Succ);
    }
  }

  // 4. Compute the exit state by meeting IN states of successors
  // Check if it's terminal BB and update ExitState if needed
  LockStateTy &ExitState = FuncStateIt->second.ExitState;
  bool FirstExitBlock = true;
  // Iterate over all blocks to find terminal ones
  for (const BasicBlock &BB : *F) {
    if (succ_empty(&BB)) {
      const auto OutIt = OutStates.find(&BB);
      if (OutIt != OutStates.end()) {
        if (FirstExitBlock) {
          ExitState = OutIt->second; // First exit block, initialize exit state
          FirstExitBlock = false;
        } else {
          // Intersect meetState with predOutState
          ExitState = intersectLockStates(ExitState, OutIt->second);
        }
      }
    }
  }

  // Traverse CFG in reverse post-order
  // ReversePostOrderTraversal<const Function *> RPOT(&F);
  // for (const BasicBlock *BB : RPOT) {
  //   WorkList.push_back(BB);
  // }
}

void LockOwnershipInfo::doIPALockOwnershipAnalysis(bool InstrToLockFlag) {
  for (auto It = scc_begin(CG); !It.isAtEnd(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;
    assert(!SCC.empty() && "SCC with no functions?");

    // Process SCC
    bool Changed = false;
    do {
      Changed = false;
      for (const CallGraphNode *CGN : SCC) {
        const auto *F = CGN->getFunction();
        if (!F || F->isDeclaration())
          continue;

        // Store the previous function exit state
        LockStateTy PrevExitState;
        const auto FuncStateIt = FuncStates.find(F);
        if (FuncStateIt != FuncStates.end())
          PrevExitState = FuncStateIt->second.ExitState;

        // Analyze function and build function state
        buildSummary(F, InstrToLockFlag);

        // Check if ExitState changed
        if (FuncStates[F].ExitState != PrevExitState)
          Changed = true;
      }
    } while (Changed);
  }
}

bool LockOwnershipInfo::loadSummary() {
  std::ifstream Summary;
  if (!openSummary(Summary, LockOwnershipSummaryFileName))
    return false;
  std::string Line;
  while (std::getline(Summary, Line))
    if (const GlobalVariable *GV = M.getGlobalVariable(Line, true))
      if (!GV->hasLocalLinkage())
        ProtectedGVs.insert(GV);
  return true;
}

void LockOwnershipInfo::writeSummary() const {
  std::ofstream Summary(tsanSummaryDir() + "/" + LockOwnershipSummaryFileName);
  if (!Summary.is_open()) {
    errs() << "Error: Could not open file " << LockOwnershipSummaryFileName
           << " for writing\n";
    return;
  }
  LLVM_DEBUG(dbgs() << "Writing analysis results to "
                    << LockOwnershipSummaryFileName << "\n");

  Summary << summaryHeader();
  std::vector<std::string> Lines;
  for (const GlobalVariable *GV : ProtectedGVs)
    if (GV->hasName() && !GV->hasLocalLinkage())
      Lines.push_back(GV->getName().str());
  llvm::sort(Lines);
  for (const auto &L : Lines)
    Summary << L << "\n";
  Summary.close();
}

/// True if the module asks TSan to stop tracking synchronization anywhere.
///
/// Inside such a region a mutex still excludes, but it no longer establishes
/// happens-before, so two threads holding the same lock genuinely race and
/// TSan is expected to say so (compiler-rt/test/tsan/ignore_sync.cpp). No
/// amount of static lockset reasoning can see that, and concluding a global is
/// protected would suppress exactly the report the annotation exists to
/// produce. So in a module that uses it, we conclude nothing.
static bool moduleIgnoresSync(const Module &M) {
  // Every match, not just the first: returning on the first candidate answered
  // "no" whenever that one happened to be unused and a later one was not.
  for (const Function &F : M.functions()) {
    const StringRef Name = F.getName();
    if ((Name.contains("AnnotateIgnoreSyncBegin") ||
         Name.contains("__tsan_ignore_thread_sync_begin")) &&
        !F.use_empty())
      return true;
  }
  return false;
}

LockOwnershipInfo::LockOwnershipInfo(CallGraph &CG_, Module &MM_,
                                     SingleThreadedInfo &STI)
    : M(MM_), CG(&CG_) {
  if (moduleIgnoresSync(M)) {
    LLVM_DEBUG(dbgs() << "Module ignores synchronization; lock ownership "
                         "concludes nothing\n");
    return;
  }

  if (!findLockUnlockFunctions())
    return;

  // Get top-down callgraph list and traverse it
  // const auto SCCList = getTopDownSCCList(CG);

  computeReleaseSummaries();
  doIPALockOwnershipAnalysis(false);
  doIPALockOwnershipAnalysis(true);

  findProtectedGlobalVariables(STI);
  if (TsanUseAnalysisSummaries)
    SummaryLoaded = loadSummary();
  // Never over the summary this compile was seeded from.
  if (TsanUseAnalysisSummaries && !SummaryLoaded)
    writeSummary();
}

bool LockOwnershipInfo::isAnnotationFunc(const Function *F) {
  // The __tsan_mutex_* family are annotations, not operations: whether
  // __tsan_mutex_pre_lock acquires anything depends on flags passed alongside
  // it (__tsan_mutex_try_lock, __tsan_mutex_read_lock, and
  // __tsan_mutex_try_lock_failed on the post_lock call). Their names contain
  // "lock", so pattern matching took every one of them for an acquisition --
  // including the pre_unlock ones, and including a try-lock that failed.
  // Modelling the flags properly is possible; until then they hold nothing.
  return F && F->getName().starts_with("__tsan_mutex_");
}

bool LockOwnershipInfo::isTryLockFunc(const Function *F) {
  if (!F)
    return false;
  const StringRef Name = F->getName();
  // A conditional acquisition: the call returns without the lock on some
  // path that the program is expected to take. Try-locks fail whenever the
  // mutex is held; timed and clock variants fail with ETIMEDOUT as part of
  // their contract. The runtime records the lock only when the call
  // succeeds, so counting these as held made every access on the failure
  // path look protected.
  return Name.contains("trylock") || Name.contains("try_lock") ||
         Name.contains("tryrdlock") || Name.contains("trywrlock") ||
         Name.contains("TryLock") || Name.contains("timedlock") ||
         Name.contains("timedrdlock") || Name.contains("timedwrlock") ||
         Name.contains("clocklock") || Name.contains("clockrdlock") ||
         Name.contains("clockwrlock");
}

bool LockOwnershipInfo::isSharedLockFunc(const Function *F) {
  if (!F)
    return false;
  const StringRef Name = F->getName();
  // No bare "rlock": pthread_rwlock_w[rlock] contains it, so every rwlock
  // *writer* acquisition matched and was recorded as holding nothing, which
  // switched this analysis off for rwlock-based code entirely.
  static constexpr StringRef SharedPatterns[] = {"rdlock", "lock_shared",
                                                 "shared_lock", "read_lock"};
  for (const StringRef Pattern : SharedPatterns)
    if (Name.contains(Pattern))
      return true;
  return false;
}

bool LockOwnershipInfo::findLockUnlockFunctions() {
  // Exact names only. A substring match ("lock") made every function whose
  // name merely contains it -- flock, memblock_get ("block" contains "lock"),
  // a user block_alloc -- an acquisition, and if its first argument resolved
  // to a global it was recorded as a lock held for the rest of the function,
  // so accesses under it looked protected when nothing was held. The set is
  // the standard C/C++/POSIX/C11 primitives, matched by full name.
  static const StringSet<> LockNames = {"pthread_mutex_lock",
                                        "pthread_mutex_trylock",
                                        "pthread_mutex_timedlock",
                                        "pthread_spin_lock",
                                        "pthread_spin_trylock",
                                        "pthread_rwlock_rdlock",
                                        "pthread_rwlock_tryrdlock",
                                        "pthread_rwlock_timedrdlock",
                                        "pthread_rwlock_wrlock",
                                        "pthread_rwlock_trywrlock",
                                        "pthread_rwlock_timedwrlock",
                                        "mtx_lock",
                                        "mtx_trylock",
                                        "mtx_timedlock",
                                        "omp_set_lock",
                                        "omp_set_nest_lock"};

  static const StringSet<> UnlockNames = {
      "pthread_mutex_unlock", "pthread_spin_unlock", "pthread_rwlock_unlock",
      "mtx_unlock",           "omp_unset_lock",      "omp_unset_nest_lock"};
  auto matchPatterns = [](const Function &F, StringRef CurrFuncName,
                          const StringSet<> &FuncNames,
                          SmallPtrSet<const Function *, 4> &FuncSet) {
    if (F.arg_size() > 0 && FuncNames.contains(CurrFuncName)) {
      FuncSet.insert(&F);
      LLVM_DEBUG(dbgs() << "Found lock function: " << F.getName() << "\n");
    }
  };

  // Find all functions that match lock/unlock patterns.
  //
  // Unlocks are classified first, and a function recognised as one is never
  // also treated as an acquisition. The lock patterns include the bare
  // substring "lock", and every unlock name contains it -- "pthread_mutex_
  // unlock" quite literally has "lock" in it -- so with both sets filled
  // independently and getLockCallInfo testing the lock set first, releases
  // were read as acquisitions and the lock was never let go.
  for (const Function &F : M.functions()) {
    if (isAnnotationFunc(&F))
      continue;
    matchPatterns(F, F.getName(), UnlockNames, UnlockFuncs);
    if (!UnlockFuncs.contains(&F))
      matchPatterns(F, F.getName(), LockNames, LockFuncs);
  }

  if (LockFuncs.empty() || UnlockFuncs.empty()) {
    // Not a warning: most translation units simply do not lock anything, and
    // this printed to stderr on every one of them.
    LLVM_DEBUG(dbgs() << "No lock/unlock functions in module " << M.getName()
                      << "; lock ownership concludes nothing\n");
    return false;
  }
  return true;
}

SmallPtrSet<const Value *, 4>
LockOwnershipInfo::getLocksProtecting(const Instruction *I) const {
  const auto It = InstrToLockMap.find(I);
  if (It != InstrToLockMap.end()) {
    for (auto *Lock : It->second) {
      LLVM_DEBUG(dbgs() << "  Lock: " << *Lock << "\n");
    }
    return It->second;
  }
  LLVM_DEBUG(dbgs() << "  No locks found for instruction: " << *I << "\n");
  return {};
}

/// True when \p I reads or writes \p GV through its own pointer operand
/// (possibly via a constant expression), i.e. the access is one this scan
/// classifies. Any other use of the address is an escape.
static bool isDirectAccessOf(const Instruction *I, const GlobalVariable *GV) {
  const Value *Ptr = nullptr;
  if (const auto *LI = dyn_cast<LoadInst>(I))
    Ptr = LI->getPointerOperand();
  else if (const auto *SI = dyn_cast<StoreInst>(I))
    Ptr = SI->getPointerOperand();
  else
    return false;
  return Ptr->stripPointerCastsAndAliases() == GV ||
         getUnderlyingObject(Ptr) == GV;
}

void LockOwnershipInfo::findProtectedGlobalVariables(SingleThreadedInfo &STI) {
  LLVM_DEBUG(
      dbgs() << "\n@@@@@@@@@ Finding protected global variables @@@@@@@@@\n");
  for (const GlobalVariable &GV : M.globals()) {
    if (GV.user_empty() || GV.isConstant())
      continue;
    // Only a variable this module owns outright can be judged protected: an
    // external one is accessed by other translation units under locks (or
    // none) that this module never sees, and a declaration has no accesses
    // here at all. Same rule as the single-writer analysis.
    if ((!TsanWholeProgram && !GV.hasLocalLinkage()) || GV.isDeclaration())
      continue;

    LLVM_DEBUG(dbgs() << "\nChecking GV: " << GV.getName() << "\n");
    SmallPtrSet<const Value *, 4> CommonLocksForGV;
    bool IsFirstAccess = true;
    bool AllAccessesProtected = true;

    SmallVector<const Instruction *, 8> Accesses;
    SmallPtrSet<const Value *, 8> VisitedUsers;
    collectAccessingInstrs(&GV, Accesses, VisitedUsers);

    for (const Instruction *I : Accesses) {
      // Don't consider accesses in single-threaded functions
      if (STI.isSingleThreaded(I->getFunction())) {
        LLVM_DEBUG(dbgs() << "  Function: " << I->getFunction()->getName()
                          << " ST\n");
        continue;
      }
      LLVM_DEBUG(dbgs() << "  Function: " << I->getFunction()->getName()
                        << " MT\n");

      LLVM_DEBUG(dbgs() << "\tAccess: " << *I << "\n");
      // The address of the variable leaving through anything other than the
      // pointer operand of a load or store -- passed to a call, stored into
      // another object, cast to an integer, merged in a phi -- means it is
      // read and written through paths this scan does not see, under
      // whatever lock they please. That is an escape, not an access.
      if (!isDirectAccessOf(I, &GV)) {
        LLVM_DEBUG(dbgs() << "\t\tAddress escapes here\n");
        AllAccessesProtected = false;
        break;
      }
      const auto LocksForCurrentAccess = getLocksProtecting(I);

      if (LocksForCurrentAccess.empty()) {
        LLVM_DEBUG(dbgs() << "\t\tNo locks found for this access in function: "
                          << I->getFunction()->getName() << "\n");
        AllAccessesProtected = false;
        break;
      }

      if (IsFirstAccess) {
        LLVM_DEBUG(dbgs() << "\t\tFirst access, setting common locks\n");
        CommonLocksForGV = LocksForCurrentAccess;
        IsFirstAccess = false;
      } else {
        // Find an intersection between the current set of common locks
        // and locks for this access.
        SmallPtrSet<const Value *, 4> NewCommonLocks;
        for (const Value *Lock : CommonLocksForGV)
          if (LocksForCurrentAccess.contains(Lock))
            NewCommonLocks.insert(Lock);
        CommonLocksForGV = NewCommonLocks;

        if (CommonLocksForGV.empty()) {
          // No common locks found for all accesses
          AllAccessesProtected = false;
          break;
        }
      }
    }

    // Requiring IsFirstAccess to be false is the point: without it, a global
    // whose accesses were all skipped above -- every one in a single-threaded
    // function, or previously every one behind a ConstantExpr -- was declared
    // protected on the strength of having been looked at, not of any lock
    // having been found.
    if (AllAccessesProtected && !IsFirstAccess && !CommonLocksForGV.empty()) {
      LLVM_DEBUG(
          dbgs()
          << "\t\tAll accesses are protected, adding GV to protected list\n");
      ProtectedGVs.insert(&GV);
    }
  }
}

void LockOwnershipInfo::print(raw_ostream &OS) const {
  /*
  auto PrintState = [&](const LockStateTy &State) {
    for (const auto &LockEntry : State) {
      OS << "  Mutex:       " << *LockEntry.first << "\n";
      if (LockEntry.second.IsLocked)
        OS << "  Locked at: " << *LockEntry.second.LockInstr << "\n";
      else
        OS << "  Unlocked\n";
      OS << "  .............................\n";
    }
  };

  OS << "============= Lock Ownership Analysis =============\n";
  OS << "Module: " << M.getName() << "\n";
  OS << "===============================================\n\n";

  OS << "########## Lock/Unlock Pairs Found ###########n";
  for (const auto &[LockInstr, UnlockInstr] : LockUnlockPairs) {
    OS << "----------------------------------------\n";
    OS << "Lock:   " << *LockInstr << "\n";
    if (UnlockInstr)
      OS << "Unlock: " << *UnlockInstr << "\n";
    else
      OS << "Unlock: ambiguous (different lock instructions at merge)\n";
  }
  OS << "##########################################\n\n";

  for (const auto &FuncEntry : FuncStates) {
    OS << "\n======== Lock States by Basic Block for Function "
       << FuncEntry.first->getName() << " =========\n";
    for (const auto &[BB, State] : FuncEntry.second.OutStates) {
      if (State.empty())
        continue;
      OS << "----------------------------------------\n";
      OS << "Basic Block: ";
      if (BB->hasName())
        OS << BB->getName();
      else
        OS << "[unnamed]";
      OS << "\n";

      PrintState(State);
    }
    OS << "===========================================================\n\n";

    OS << "\n%%%%%%%%%%%%% Exit State for Function "
       << FuncEntry.first->getName() << " %%%%%%%%%%%%%%\n";
    if (FuncEntry.second.ExitState.empty()) {
      OS << "  <empty>\n";
    } else {
      PrintState(FuncEntry.second.ExitState);
    }
    OS << "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n\n";
  }

  OS << "\nInstruction to Lock Map:\n";
  if (InstrToLockMap.empty()) {
    OS << "  (empty)\n";
    return;
  }

  for (const auto &Pair : InstrToLockMap) {
    const Instruction *Inst = Pair.first;
    const auto &Locks = Pair.second;

    OS << "  Instruction: ";
    Inst->print(OS);
    OS << "\n";

    OS << "    Locks Held: {";
    bool FirstLock = true;
    for (const Value *Lock : Locks) {
      if (!FirstLock)
        OS << ", ";
      if (Lock->hasName())
        OS << Lock->getName();
      else
        Lock->print(OS);
      FirstLock = false;
    }
    OS << "}\n";
  }
  */

  OS << "\n@@@@@@@@@ Protected Global Variables @@@@@@@@@\n";
  OS << "===============================================\n";
  if (ProtectedGVs.empty()) {
    OS << "  (empty)\n";
    return;
  }

  for (const GlobalVariable *GV : ProtectedGVs) {
    OS << "  * ";
    if (GV->hasName())
      OS << GV->getName();
    else
      GV->print(OS);
    OS << "\n";
  }
  OS << "===============================================\n";
}

AnalysisKey LockOwnership::Key;

LockOwnership::Result LockOwnership::run(Module &M, ModuleAnalysisManager &AM) {
  auto &CG = AM.getResult<CallGraphAnalysis>(M);
  return LockOwnershipInfo(CG, M, AM.getResult<SingleThreaded>(M));
}

PreservedAnalyses
LockOwnershipPrinterPass::run(Module &M, ModuleAnalysisManager &AM) const {
  OS << "Printing analysis 'Lock Ownership' for module '" << M.getName()
     << "':\n";
  AM.getResult<LockOwnership>(M).print(OS);
  return PreservedAnalyses::all();
}