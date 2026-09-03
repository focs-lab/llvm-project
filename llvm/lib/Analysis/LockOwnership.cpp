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
#include "llvm/IR/CFG.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/ValueTracking.h"
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
        LLVM_DEBUG(dbgs() << "  Pred lock instr: " << *PredIt->second.LockInstr
                          << "\n");
        LLVM_DEBUG(dbgs() << "  Current lock instr: " << *State.LockInstr
                          << "\n");

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
    if (isLockFunc(CalledFunc) && !isSharedLockFunc(CalledFunc) &&
        !isTryLockFunc(CalledFunc))
      return {LockCallType::LOCK, getUnderlyingObject(CB->getArgOperand(0))};
    if (isUnLockFunc(CalledFunc))
      return {LockCallType::UNLOCK, getUnderlyingObject(CB->getArgOperand(0))};
  }
  return {LockCallType::NONE, nullptr};
}

void LockOwnershipInfo::handleLock(LockStateTy &CurrState,
                                   const Instruction &Instr,
                                   const Value *Lock) {
  LLVM_DEBUG(dbgs() << "\nLOCK Instr: " << Instr << "\n");
  const auto It = CurrState.find(Lock);
  if (It != CurrState.end()) {
    // Already locked! Potential double lock.
    LLVM_DEBUG(dbgs() << "WARNING: Double lock on mutex: " << *Lock << "\n");
    // Keep tracking the *first* lock instruction for this path
    // It->second.LockInstr = &Instr;
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
    // Was locked, now unlocked. Record the pair.
    LLVM_DEBUG(dbgs() << "Lock: " << *Lock << "\n");
    assert(It->second.LockInstr && "LockInstr should be set");
    LLVM_DEBUG(dbgs() << "Lock Instr: " << *It->second.LockInstr << "\n\n");

    LockUnlockPairs.insert({It->second.LockInstr, &Instr});
    // Remove locks, for which we found pairs
    CurrState.erase(It);
  } else {
    // Unlocking a mutex that wasn't locked (or state diverged earlier)
    LLVM_DEBUG(dbgs() << "WARNING: Unlocking a mutex that wasn't locked: "
                      << *Lock << "\n");
    CurrState[Lock] = {false, nullptr};
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
    if (CallType == LockCallType::LOCK && Lock) {
      handleLock(InState, I, Lock);
      continue;
    }

    if (CallType == LockCallType::UNLOCK && Lock) {
      handleUnlock(InState, I, Lock);
      continue;
    }

    // 2. Process calls
    if (const auto *Call = dyn_cast<CallBase>(&I)) {
      const auto *CalledFunc = Call->getCalledFunction();
      if (!CalledFunc || CalledFunc->isDeclaration())
        continue;

      // Get a function state if it exists
      const auto FuncStateIt = FuncStates.find(CalledFunc);
      if (FuncStateIt != FuncStates.end()) {
        // Update the current state with callee's exit state
        for (const auto &[Lock, State] : FuncStateIt->second.ExitState) {
          if (State.IsLocked)
            handleLock(InState, I, Lock);
          else
            handleUnlock(InState, I, Lock);
        }
      }
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
      if (!HeldLocks.empty())
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

  if (InstrToLockFlag) {
    // Here we must iterate through all BBs, so need to add all of them
    for (const BasicBlock &BB : *F)
      WorkList.push_back(&BB);
  } else {
    WorkList.push_back(&EntryBB);
  }

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

void LockOwnershipInfo::readSummary() {
  // Clear existing analysis results
  ProtectedGVs.clear();

  std::ifstream Summary(SummaryDirName + "/" + LockOwnershipSummaryFileName);
  if (!Summary.is_open()) {
    errs() << "Error: Could not open file " << LockOwnershipSummaryFileName
           << " for reading\n";
    return;
  }
  LLVM_DEBUG(dbgs() << "Reading analysis results from "
                    << LockOwnershipSummaryFileName << "\n");

  std::string Line;

  while (std::getline(Summary, Line)) {
    if (const GlobalVariable *GV = M.getGlobalVariable(Line))
      ProtectedGVs.insert(GV);
  }

  Summary.close();
}

void LockOwnershipInfo::writeSummary() const {
  std::ofstream Summary(SummaryDirName + "/" + LockOwnershipSummaryFileName);
  if (!Summary.is_open()) {
    errs() << "Error: Could not open file " << LockOwnershipSummaryFileName
           << " for writing\n";
    return;
  }
  LLVM_DEBUG(dbgs() << "Writing analysis results to "
                    << LockOwnershipSummaryFileName << "\n");

  for (const GlobalVariable *GV : ProtectedGVs)
    Summary << GV->getName().str() << "\n";

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

  doIPALockOwnershipAnalysis(false);
  doIPALockOwnershipAnalysis(true);

  findProtectedGlobalVariables(STI);
  if (TsanUseAnalysisSummaries)
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
  return Name.contains("trylock") || Name.contains("try_lock") ||
         Name.contains("tryrdlock") || Name.contains("trywrlock") ||
         Name.contains("TryLock");
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
  // Common lock function names and patterns
  const SmallVector<StringRef> LockNames = {"pthread_mutex_lock",
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
                                            "_mutex_lock",
                                            "spinlock_lock",
                                            "acquire_lock",
                                            "rwlock_rdlock",
                                            "rwlock_wrlock",
                                            "lock"};

  const SmallVector<StringRef> UnlockNames = {
      "pthread_mutex_unlock",  "pthread_spin_unlock",
      "pthread_rwlock_unlock", "mtx_unlock",
      "_mutex_unlock",         "spinlock_unlock",
      "release_lock",          "rwlock_unlock", "unlock"};
  auto matchPatterns = [](const Function &F, StringRef CurrFuncName,
                          const SmallVector<StringRef> &FuncNames,
                          SmallPtrSet<const Function *, 4> &FuncSet) {
    for (const auto &FuncName : FuncNames) {
      if (CurrFuncName.contains(FuncName) && F.arg_size() > 0) {
        FuncSet.insert(&F);
        LLVM_DEBUG(dbgs() << "Found function matching pattern '" << FuncName
                          << "': " << F.getName() << "\n");
        return;
      }
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

void LockOwnershipInfo::findProtectedGlobalVariables(SingleThreadedInfo &STI) {
  LLVM_DEBUG(
      dbgs() << "\n@@@@@@@@@ Finding protected global variables @@@@@@@@@\n");
  for (const GlobalVariable &GV : M.globals()) {
    if (GV.user_empty() || GV.isConstant())
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
  if (std::ifstream SummaryFile(SummaryDirName + "/" +
                                LockOwnershipSummaryFileName);
      TsanUseAnalysisSummaries && SummaryFile.good()) {
    LLVM_DEBUG(dbgs() << "Found existing summary file for LockOwnership "
                         "Analysis. Loading results.\n");
    SummaryFile.close();
    return LockOwnershipInfo(M);
  }
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