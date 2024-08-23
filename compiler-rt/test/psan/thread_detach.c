// RUN: %clang_psan -O1 %s -o %t && %run %t 2>&1 | FileCheck %s
#include "test.h"

void *Thread(void *x) {
  barrier_wait(&barrier);
  return 0;
}

int main() {
  barrier_init(&barrier, 2);
  pthread_t t;
  pthread_create(&t, 0, Thread, 0);
  barrier_wait(&barrier);
  pthread_detach(t);
  fprintf(stderr, "PASS\n");
  return 0;
}

// CHECK-NOT: WARNING: PredictiveSanitizer: thread leak
// CHECK: PASS
