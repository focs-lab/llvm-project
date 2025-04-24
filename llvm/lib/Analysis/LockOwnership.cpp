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

#include "llvm/IR/CFG.h"
#include "llvm/Analysis/LockOwnership.h"

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
    // Check if this mutex exists and has the *exact same* state in
    // predOutState
    auto PredIt = PredOutState.find(Lock);
    if (PredIt != PredOutState.end() &&
        PredIt->second == State /* && State.IsLocked */) {
      // Only keep if state is identical in both
      NextMeetState[Lock] = State;
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
    const LockStateTy& PredOutState = It->second;

    if (FirstPred) {
      MeetState = PredOutState; // Initialize with the first predecessor's state
      FirstPred = false;
    } else {
      // Intersect MeetState with PredOutState
      MeetState = intersectLockStates(MeetState, PredOutState);
    }
  }

  // If a block has no predecessors (entry block), the meet result is an empty map (initial state)
  if (pred_empty(BB))
    return LockStateTy(); // Initial state: nothing locked

  return MeetState;
}

std::pair<LockOwnershipInfo::LockCallType, Value *>
LockOwnershipInfo::getLockCallInfo(const Instruction *I) {
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
                                     const Instruction &Inst, const Value *Lock) {
  const auto It = CurrState.find(Lock);
  if (It != CurrState.end()) {
    // Was locked, now unlocked. Record the pair.
    dbgs() << "Found lock/unlock pair: " << *It->second.LockInstr << "\n";
    LockUnlockPairs.insert({It->second.LockInstr, &Inst});
    // Remove locks, for which we found pairs
    CurrState.erase(It);
  } else {
    // Unlocking a mutex that wasn't locked (or state diverged earlier)
    // dbgs() << "Potential unlock of mutex that is not held\n";
    CurrState[Lock] = {false, nullptr};
  }
}

/// Transfer Function: Applies block's instructions to the in-state
/// Returns true if the out-state *changes* as a result
bool LockOwnershipInfo::applyTransferFunction(const BasicBlock *BB,
                                              LockStateTy &CurrState,
                                              BBStateMap &OutStates) {
  for (const Instruction &Inst : *BB) {
    dbgs() << "\tInstr: " << Inst << "\n";

    auto [CallType, Lock] = getLockCallInfo(&Inst);

    // Process lock acquire and release
    if (CallType == LockCallType::LOCK && Lock) {
      handleLock(CurrState, Inst, Lock);
      continue;
    }

    if (CallType == LockCallType::UNLOCK && Lock) {
      handleUnlock(CurrState, Inst, Lock);
      continue;
    }

    // Process calls
    const auto *Call = dyn_cast<CallBase>(&Inst);
    if (!Call)
      continue;
    const auto *CalledFunc = Call->getCalledFunction();
    if (!CalledFunc || !CalledFunc->isDeclaration())
      continue;

    // Get a function state if it exists
    const auto FuncStateIt = FuncStates.find(CalledFunc);
    if (FuncStateIt != FuncStates.end()) {
      // Update current state with callee's exit state
      for (const auto &[Lock, State] : FuncStateIt->second.ExitState) {
        if (State.IsLocked)
          handleLock(CurrState, Inst, Lock);
        else
          handleUnlock(CurrState, Inst, Lock);
      }
    }
  }

  // Check if the calculated out-state differs from the stored one
  auto &OutState = OutStates[BB]; // Will insert if not present
  if (OutState != CurrState) {
    OutState = CurrState; // Update the stored state
    return true;          // State changed
  }
  return false; // State did not change
}

void LockOwnershipInfo::buildSummary(const Function *F) {
  dbgs() << "\nBuilding summary for function: " << F->getName() << "\n";

  auto [It, _] = FuncStates.insert({F, FuncStateTy()});
  BBStateMap &InStates = It->second.InStates;
  BBStateMap &OutStates = It->second.OutStates;
  LockStateTy &ExitState = It->second.ExitState;

  std::deque<const BasicBlock *> WorkList;
  SmallPtrSet<const BasicBlock *, 8> Visited;

  const BasicBlock &EntryBB = F->getEntryBlock();

  WorkList.push_back(&EntryBB);
  bool FirstPred = true;

  while (!WorkList.empty()) {
    const BasicBlock *BB = WorkList.front();
    dbgs() << "\nProcessing BB: " << BB->getName() << "\n";

    WorkList.pop_front();

    // 1. Compute IN state by meeting OUT states of predecessors
    // Skip for entry block as it's already initialized
    if (BB != &EntryBB)
      InStates[BB] = computeMeet(BB, OutStates);

    // 2. Apply transfer function to get OUT state
    LockStateTy CurrOutState = InStates[BB]; // Start with IN state
    // Modifies currentOutState in place
    bool StateChanged = applyTransferFunction(BB, CurrOutState, OutStates);

    // 3. If OUT state changed, add successors to the worklist
    if (StateChanged) {
      // Check if it's terminal BB, and update ExitState if needed
      if (succ_empty(BB)) {
        if (FirstPred) {
          ExitState = CurrOutState; // First exit block, initialize exit state
          FirstPred = false;
        } else {
          // Intersect meetState with predOutState
          ExitState = intersectLockStates(ExitState, CurrOutState);
        }
        continue;
      }

      for (const BasicBlock *Succ : successors(BB)) {
        // Add to worklist if not already processed enough or state might change
        // Simple approach: always add if state changed. Visited set prevents
        // infinite loops. A more refined approach checks if the *input* to the
        // successor would change.
        if (Visited.find(Succ) == Visited.end() ||
            StateChanged) { // Re-add if predecessor changed
          WorkList.push_back(Succ);
          Visited.insert(Succ);
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

LockOwnershipInfo::LockOwnershipInfo(CallGraph &CG, Module &M_) : M(M_) {
  if (!findPthreadFunctions())
    return;

  // Get top-down callgraph list and traverse it
  // const auto SCCList = getTopDownSCCList(CG);
  // for (auto It = SCCList.rbegin(); It != SCCList.rend(); ++It) {
  //   const std::vector<CallGraphNode *> &SCC = *It;
  //   dbgs() << "\nSSC: ";
  //   for (const CallGraphNode *CGN : SCC) {
  //     const Function *F = CGN->getFunction();
  //     if (!F || F->isDeclaration())
  //       continue;
  //     if (F->hasName())
  //       dbgs() << "Func: " << F->getName() << "\n";
  //   }
  // }

  for (auto It = scc_begin(&CG); !It.isAtEnd(); ++It) {
    const std::vector<CallGraphNode *> &SCC = *It;
    assert(!SCC.empty() && "SCC with no functions?");
    for (const CallGraphNode *CGN : SCC) {
      const auto *F = CGN->getFunction();
      if (!F || F->isDeclaration())
        continue;
      buildSummary(F);
    }
  }

  // for (const Function &F : M) {
  //   if (F.isDeclaration() || F.empty())
  //     continue;
  //   buildSummary(F);
  // }
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
      dbgs() << "  Mutex:       " << *LockEntry.first << "\n";
      if (LockEntry.second.IsLocked)
        dbgs() << "  Locked at: " << *LockEntry.second.LockInstr << "\n";
      else
        dbgs() << "  Unlocked\n";
      dbgs() << "  .............................\n";
    }
  };

  dbgs() << "============= Lock Ownership Analysis =============\n";
  dbgs() << "Module: " << M.getName() << "\n";
  dbgs() << "===============================================\n\n";

  dbgs() << "########## Lock/Unlock Pairs Found ###########n";
  for (const auto &[LockInstr, UnlockInstr] : LockUnlockPairs) {
    dbgs() << "----------------------------------------\n";
    dbgs() << "Lock:   " << *LockInstr << "\n";
    dbgs() << "Unlock: " << *UnlockInstr << "\n";
  }
  dbgs() << "##########################################\n\n";

  for (const auto &FuncEntry : FuncStates) {
    dbgs() << "\n======== Lock States by Basic Block for Function "
           << FuncEntry.first->getName() << " =========\n";
    for (const auto &[BB, State] : FuncEntry.second.OutStates) {
      if (State.empty())
        continue;
      dbgs() << "----------------------------------------\n";
      dbgs() << "Basic Block: ";
      if (BB->hasName())
        dbgs() << BB->getName();
      else
        dbgs() << "[unnamed]";
      dbgs() << "\n";

      PrintState(State);
    }
    dbgs() << "===========================================================\n\n";

    dbgs() << "\n%%%%%%%%%%%%% Exit State for Function "
           << FuncEntry.first->getName() << " %%%%%%%%%%%%%%\n";
    if (FuncEntry.second.ExitState.empty()) {
      dbgs() << "  <empty>\n";
    } else {
      PrintState(FuncEntry.second.ExitState);
    }
    dbgs() << "%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%\n\n";
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
