// Post-dominance elimination end to end: the earlier of two writes to the
// same location in one straight-line thread body is dropped in favour of
// the later one, and the race with the other thread is still reported.
//
// RUN: %clangxx_tsan -O1 %s -o %t -mllvm -tsan-use-dominance-analysis-postdom && %deflake %run %t 2>&1 | FileCheck %s
#include "test.h"

int Global;

void *Thread(void *x) {
  barrier_wait(&barrier);
  Global = 1;
  Global = 2;
  return NULL;
}

int main() {
  barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, NULL, Thread, NULL);
  Global = 3;
  barrier_wait(&barrier);
  pthread_join(t, NULL);
  fprintf(stderr, "DONE\n");
  return 0;
}

// CHECK: WARNING: ThreadSanitizer: data race
// CHECK: DONE
