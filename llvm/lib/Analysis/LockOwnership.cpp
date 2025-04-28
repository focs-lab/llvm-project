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
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"

using namespace llvm;

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
      else
        // This unlock point is no more unambiguous now because of merge
        // of control flows
        NextMeetState[Lock] = {State.IsLocked, nullptr};
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

std::pair<LockOwnershipInfo::LockCallType, Value *>
LockOwnershipInfo::getLockCallInfo(const Instruction *I) const {
  if (!I)
    return {LockCallType::NONE, nullptr};

  if (const auto *CS = dyn_cast<CallBase>(I)) {
    Function *CalledFunc = CS->getCalledFunction();
    if (!CalledFunc || !CalledFunc->isDeclaration() ||
        (CS->getNumOperands() == 0))
      return {LockCallType::NONE, nullptr};

    if (CalledFunc == LockFunc)
      return {LockCallType::LOCK, CS->getArgOperand(0)};
    if (CalledFunc == UnlockFunc)
      return {LockCallType::UNLOCK, CS->getArgOperand(0)};
  }
  return {LockCallType::NONE, nullptr};
}

void LockOwnershipInfo::handleLock(LockStateTy &CurrState,
                                   const Instruction &Inst, const Value *Lock) {
  const auto It = CurrState.find(Lock);
  if (It != CurrState.end()) {
    // Already locked! Potential double lock.
    dbgs() << "Warning: Double lock on mutex: " << *Lock << "\n";
    // Keep tracking the *latest* lock instruction for this path
    It->second.LockInstr = &Inst;
  } else {
    // Not locked or not present, mark as locked
    CurrState[Lock] = {true, &Inst};
  }
}

void LockOwnershipInfo::handleUnlock(LockStateTy &CurrState,
                                     const Instruction &Inst,
                                     const Value *Lock) {
  const auto It = CurrState.find(Lock);
  if (It != CurrState.end()) {
    // Was locked, now unlocked. Record the pair.
    // dbgs() << "Found lock/unlock pair: " << *It->second.LockInstr << "\n";
    LockUnlockPairs.insert({It->second.LockInstr, &Inst});
    // Remove locks, for which we found pairs
    CurrState.erase(It);
  } else {
    // Unlocking a mutex that wasn't locked (or state diverged earlier)
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
    dbgs() << "\n\tInstr: " << I << "\n";
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
          if (State.IsLocked && State.LockInstr)
            dbgs() << " at " << *State.LockInstr;
          dbgs() << "\n";

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
      dbgs() << "\tInstr to lock map: " << I << "\n";
      // Record all currently held locks for this instruction
      SmallPtrSet<const Value *, 4> HeldLocks;
      for (const auto &[Lock, State] : InState) {
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
    return true;          // State changed
  }
  return false; // State did not change
}

void LockOwnershipInfo::buildSummary(const Function *F, bool InstrToLockFlag) {
  dbgs() << "\n===============================================\n";
  dbgs() << "Building summary for function: " << F->getName() << "\n";

  auto [FuncStateIt, _] = FuncStates.try_emplace(F, FuncStateTy());
  BBStateMap &InStates = FuncStateIt->second.InStates;
  BBStateMap &OutStates = FuncStateIt->second.OutStates;

  // SmallPtrSet<const BasicBlock *, 8> Visited;
  const BasicBlock &EntryBB = F->getEntryBlock();

  std::deque<const BasicBlock *> WorkList;
  WorkList.push_back(&EntryBB);

  while (!WorkList.empty()) {
    const BasicBlock *BB = WorkList.front();
    WorkList.pop_front();
    dbgs() << "\nProcessing BB: " << BB->getName() << "\n";

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
  for (auto It = scc_begin(&CG); !It.isAtEnd(); ++It) {
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

LockOwnershipInfo::LockOwnershipInfo(CallGraph &CG_, Module &MM_)
    : M(MM_), CG(CG_) {
  if (!findPthreadFunctions())
    return;

  // Get top-down callgraph list and traverse it
  // const auto SCCList = getTopDownSCCList(CG);

  doIPALockOwnershipAnalysis(false);
  doIPALockOwnershipAnalysis(true);
}

bool LockOwnershipInfo::findPthreadFunctions() {
  LockFunc = M.getFunction("pthread_mutex_lock");
  UnlockFunc = M.getFunction("pthread_mutex_unlock");

  if (!LockFunc || !UnlockFunc) {
    errs() << "Warning: pthread_mutex_lock/pthread_mutex_unlock not found '"
           << "in the module " << M.getName() << "\n";
    return false;
  }
  return true;
}

void LockOwnershipInfo::print(raw_ostream &OS) const {
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
}

AnalysisKey LockOwnership::Key;

LockOwnership::Result LockOwnership::run(Module &M, ModuleAnalysisManager &AM) {
  return LockOwnershipInfo(AM.getResult<CallGraphAnalysis>(M), M);
}

PreservedAnalyses
LockOwnershipPrinterPass::run(Module &M, ModuleAnalysisManager &AM) const {
  OS << "Printing analysis 'Lock Ownership' for module '" << M.getName()
     << "':\n";
  AM.getResult<LockOwnership>(M).print(OS);
  return PreservedAnalyses::all();
}

#define DEBUG_TYPE "ownership"
