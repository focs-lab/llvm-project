// Races must survive dominance-based redundancy elimination.
//
// Both cases here are ones an earlier version of the analysis lost: it treated
// two accesses as interchangeable whenever they shared an underlying object,
// without checking that they were the same address and that the covering
// access was at least as wide.
//
// The barrier is TSan-invisible, so it orders the two threads in real time
// without establishing happens-before. It sits after the racing access in main
// rather than between the two accesses there, because a call the analysis
// cannot see into would block the very elimination this test is about.
//
// Cases that cannot be sequenced this way -- the loop back-edge, and
// synchronization behind a call -- are covered at IR level in
// llvm/test/Instrumentation/ThreadSanitizerNew/elim-by-dominance-paths.ll.

// RUN: %clangxx_tsan -O1 %s -o %t -mllvm -tsan-use-dominance-analysis && %deflake %run %t 2>&1 | FileCheck %s
// RUN: %clangxx_tsan_stock -O1 %s -o %t && %deflake %run %t 2>&1 | FileCheck %s
#include "test.h"

int Arr[8];
long long Wide;

// Races with main's write to Arr[3]. Arr[0] dominates that write and shares
// its underlying object, but is a different location and cannot cover it.
void *DistinctElement(void *x) {
  barrier_wait(&barrier);
  Arr[3] = 42;
  return NULL;
}

int main() {
  barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, DistinctElement, NULL);

  Arr[0] = 1; // dominator: same object as Arr[3], different location
  Arr[3] = 2; // the racing access

  barrier_wait(&barrier);
  pthread_join(t, NULL);
  return Arr[3] == 12345;
}

// CHECK: WARNING: ThreadSanitizer: data race
