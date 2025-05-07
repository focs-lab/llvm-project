//==- LockOwnership.h - --==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the generic Lock Ownership interface.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_LOCKOWNERSHIP_H
#define LLVM_ANALYSIS_LOCKOWNERSHIP_H

#include "SingleThreaded.h"

#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

const std::string LockOwnershipSummaryFileName = "lo_summary.txt";

// Represents the lock state for a single mutex
struct LockState {
  bool IsLocked = false;
  const Instruction *LockInstr = nullptr; // Where it was locked

  // Comparison operator needed for state comparison
  bool operator==(const LockState &Other) const {
    return IsLocked == Other.IsLocked && LockInstr == Other.LockInstr;
  }
  bool operator!=(const LockState &Other) const { return !(*this == Other); }
};

// Map of the lock to the instruction where it was locked
using LockStateTy = SmallDenseMap<const Value *, LockState>;

/// Interface to access safety global (interprocedural) analysis results.
class LockOwnershipInfo {
public:
  explicit LockOwnershipInfo(CallGraph &CG_, Module &M,
                             SingleThreadedInfo &STI);
  explicit LockOwnershipInfo(Module &MM) : M(MM) { readSummary(); };

  void print(raw_ostream &O) const;

  /// This is needed for using with OuterAnalysisManagerProxy
  bool invalidate(Module &, const PreservedAnalyses &,
                  ModuleAnalysisManager::Invalidator &) {
    return false;
  }

  /// Returns true if the given instruction is inside at least one critical
  /// section
  bool isInsideCriticalSection(const Instruction *I) const {
    const auto It = InstrToLockMap.find(I);
    return It != InstrToLockMap.end() && !It->second.empty();
  }

  /// Is the given global variable is protected by at least one lock
  bool isProtectedGV(const GlobalVariable *GV) const {
    return ProtectedGVs.contains(GV);
  }

private:
  Module &M;
  CallGraph *CG = nullptr;
  SmallPtrSet<const Function *, 4> LockFuncs, UnlockFuncs;

  // Whether this Instruction belongs to some critical section of some lock
  DenseMap<const Instruction *, SmallPtrSet<const Value *, 4>> InstrToLockMap;

  // List of GVs protected by at least one lock
  SmallPtrSet<const GlobalVariable *, 8> ProtectedGVs;

  bool isLockFunc(const Function *F) const { return LockFuncs.contains(F); }

  bool isUnLockFunc(const Function *F) const { return UnlockFuncs.contains(F); }

  enum class LockCallType { NONE, LOCK, UNLOCK };
  std::pair<LockCallType, const Value *>
  getLockCallInfo(const Instruction *I) const;

  // Dataflow state for each basic block
  using BBStateMap = SmallDenseMap<const BasicBlock *, LockStateTy>;
  struct FuncStateTy {
    BBStateMap InStates, OutStates;
    LockStateTy ExitState;
  };
  SmallDenseMap<const Function *, FuncStateTy> FuncStates;

  // Found lock/unlock pairs
  SmallDenseSet<std::pair<const Instruction *, const Instruction *>>
      LockUnlockPairs;

  static SmallVector<std::vector<CallGraphNode *>>
  getTopDownSCCList(CallGraph &CG);
  void buildSummary(const Function *F, bool InstrToLockFlag);

  /// Main analysis function
  void doIPALockOwnershipAnalysis(bool InstrToLockFlag);

  /// Meet Operator: Intersects the lock states from predecessors
  LockStateTy computeMeet(const BasicBlock *BB, BBStateMap &OutStates);

  /// Intersect lock states of two BBs
  LockStateTy intersectLockStates(LockStateTy MeetState,
                                  const LockStateTy &PredOutState);

  /// Transfer Function: Applies block's instructions to the in-state
  /// Returns true if the out-state *changes* as a result
  bool applyTransferFunc(const BasicBlock *BB, LockStateTy &InState,
                         LockStateTy &OutState, bool InstrToLockFlag);

  /// Process mutex lock and unlock, which we met
  void handleLock(LockStateTy &CurrState, const Instruction &Instr,
                  const Value *Lock);
  void handleUnlock(LockStateTy &CurrState, const Instruction &Instr,
                    const Value *Lock);

  /// Find pthread lock/unlock functions
  bool findLockUnlockFunctions();

  /// Analyzes the module to find global variables that are consistently
  /// protected by locks. A global variable is considered protected if all its
  /// accesses are guarded by at least one common mutex lock across all usage
  /// points.
  void findProtectedGlobalVariables(SingleThreadedInfo &STI);

  /// Returns the set of locks (mutexes) that are held when instruction I
  SmallPtrSet<const Value *, 4> getLocksProtecting(const Instruction *I) const;

  void readSummary();
  void writeSummary() const;
};

/// This pass performs the global (interprocedural) escape analysis.
class LockOwnership : public AnalysisInfoMixin<LockOwnership> {
  friend AnalysisInfoMixin<LockOwnership>;
  static AnalysisKey Key;

public:
  using Result = LockOwnershipInfo;
  static Result run(Module &M, ModuleAnalysisManager &AM);
};

/// Printer pass for the \c OwnershipAnalysis results.
class LockOwnershipPrinterPass
    : public PassInfoMixin<LockOwnershipPrinterPass> {
  raw_ostream &OS;

public:
  explicit LockOwnershipPrinterPass(raw_ostream &OS) : OS(OS) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) const;
  static bool isRequired() { return true; }
};

} // end namespace llvm

#endif // LLVM_ANALYSIS_LOCKOWNERSHIP_H
