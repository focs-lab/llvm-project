// The dynamic single-threaded guard (-tsan-use-active-thread-count) skips
// the runtime call while only one thread exists and must not skip it once a
// second thread runs: the race between the two threads below is reported.
//
// RUN: %clang_tsan -O1 %s -o %t -mllvm -tsan-use-active-thread-count && %deflake %run %t 2>&1 | FileCheck %s
#include "test.h"

int Global;

void *Thread(void *x) {
  barrier_wait(&barrier);
  Global = 42;
  return NULL;
}

int main() {
  barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, Thread, NULL);
  Global = 43;
  barrier_wait(&barrier);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE\n");
  return 0;
}

// CHECK: WARNING: ThreadSanitizer: data race
// CHECK: DONE
