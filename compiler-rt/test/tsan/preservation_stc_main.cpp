// Races involving main must survive the single-threaded-context analysis.
//
// main starts single-threaded and stops being so at pthread_create. Treating
// it as single-threaded wholesale left every access in it uninstrumented, and
// this race -- between main and the thread it had just started -- went
// unreported.

// RUN: %clangxx_tsan -O1 %s -o %t -mllvm -tsan-use-single-threaded && %deflake %run %t 2>&1 | FileCheck %s
// RUN: %clangxx_tsan_stock -O1 %s -o %t && %deflake %run %t 2>&1 | FileCheck %s
#include "test.h"

int Global;

void *Thread(void *x) {
  barrier_wait(&barrier);
  Global = 1;
  return NULL;
}

int main() {
  barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, Thread, NULL);
  Global = 2;
  barrier_wait(&barrier);
  pthread_join(t, NULL);
  return Global == 42;
}

// CHECK: WARNING: ThreadSanitizer: data race
