// RUN: %clangxx_psan -O1 %s -o %t && %deflake %run %t | FileCheck %s
// This test fails on powerpc64 big endian.
// The Psan report is returning wrong information about
// the location of the race.
// XFAIL: target=powerpc64-unknown-linux-gnu{{.*}}
#include "java.h"

void foobar() {
}

void barbaz() {
}

void *Thread(void *p) {
  barrier_wait(&barrier);
  __psan_read1_pc((jptr)p, (jptr)foobar + kPCInc);
  return 0;
}

int main() {
  barrier_init(&barrier, 2);
  int const kHeapSize = 1024 * 1024;
  jptr jheap = (jptr)malloc(kHeapSize + 8) + 8;
  __psan_java_init(jheap, kHeapSize);
  const int kBlockSize = 16;
  __psan_java_alloc(jheap, kBlockSize);
  pthread_t th;
  pthread_create(&th, 0, Thread, (void*)jheap);
  __psan_write1_pc((jptr)jheap, (jptr)barbaz + kPCInc);
  barrier_wait(&barrier);
  pthread_join(th, 0);
  __psan_java_free(jheap, kBlockSize);
  fprintf(stderr, "DONE\n");
  return __psan_java_fini();
}

// CHECK: WARNING: PredictiveSanitizer: data race
// CHECK:     #0 foobar
// CHECK:     #0 barbaz
// CHECK: DONE
