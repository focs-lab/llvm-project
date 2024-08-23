// RUN: %clangxx_psan -O1 %s -o %t && %deflake %run %t | FileCheck %s
// This test fails on powerpc64 big endian.
// The Psan report is returning wrong information about
// the location of the race.
// XFAIL: target=powerpc64-unknown-linux-gnu{{.*}}

#include "test.h"

typedef unsigned long uptr;
extern "C" void __psan_read_range_pc(uptr addr, uptr size, uptr pc);
extern "C" void __psan_write_range_pc(uptr addr, uptr size, uptr pc);

void foobar() {
}

void barbaz() {
}

void *Thread(void *p) {
  barrier_wait(&barrier);
  __psan_read_range_pc((uptr)p, 32, (uptr)foobar + kPCInc);
  return 0;
}

int main() {
  barrier_init(&barrier, 2);
  int a[128];
  pthread_t th;
  pthread_create(&th, 0, Thread, (void*)a);
  __psan_write_range_pc((uptr)(a+2), 32, (uptr)barbaz + kPCInc);
  barrier_wait(&barrier);
  pthread_join(th, 0);
  fprintf(stderr, "DONE\n");
  return 0;
}

// CHECK: WARNING: PredictiveSanitizer: data race
// CHECK:     #0 foobar
// CHECK:     #0 barbaz
// CHECK: DONE
