// The single-threaded analysis drops instrumentation for accesses that cannot
// race. That is correct about races, but TSan's use-after-free reporting rides
// on the same instrumentation and needs no second thread, so in a program that
// never creates one -- or in the single-threaded phase of one that does -- the
// report goes with it. -tsan-stc-preserve-uaf buys it back by keeping the
// instrumentation on accesses that may be to the heap.
//
// This is the positive control for the flag: the report must survive the
// analysis. free_race2.c is the same shape and opts into the same two flags
// for the same reason; the negative case -- that the report is lost without
// them -- is documented rather than tested, because a test that must fail
// under one configuration is a permanent red row in the matrix.
//
// Only one RUN line, and it names every option it depends on, including the
// one it must switch off. A line compiled with a bare %clang_tsan would not be
// the "stock TSan" control it looks like -- the harness may already have put
// analysis flags in there, and TSAN_MLLVM_FLAGS exists precisely so it can.
//
// -tsan-use-active-thread-count is disabled explicitly because it elides the
// same instrumentation at run time, where no compile-time flag can bring it
// back: with one thread live the guard is false and the call never happens.

// RUN: %clang_tsan -O1 %s -o %t -mllvm -tsan-use-single-threaded -mllvm -tsan-stc-preserve-uaf -mllvm -tsan-use-active-thread-count=false && %deflake %run %t | FileCheck %s

#include <stdlib.h>

__attribute__((noinline)) void release(void *mem) { free(mem); }

__attribute__((noinline)) void touch(void *mem) { *(long *)mem = 42; }

int main() {
  void *mem = malloc(100);
  release(mem);
  touch(mem); // use after free, on the only thread there is
  return 0;
}

// CHECK: WARNING: ThreadSanitizer: heap-use-after-free
// CHECK:   Write of size 8 at {{.*}} by main thread:
// CHECK:     #0 touch
