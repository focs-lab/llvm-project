// This program never creates a thread, so the single-threaded analysis is
// right that nothing here can race -- but the use-after-free it reports rides
// on the same instrumentation, and the analysis (and its run-time twin, the
// active-thread-count guard) would drop it. Use-after-free is outside the
// zero-loss rule, which covers data races, and the analysis stays on by
// default; this test opts out of it so the report it exists to check survives.
// Both flags are named here so the test means the same thing whatever the
// harness injects.
// RUN: %clang_tsan -O1 %s -o %t -mllvm -tsan-stc-preserve-uaf -mllvm -tsan-use-active-thread-count=false && %deflake %run %t | FileCheck %s
// RUN: %clang_tsan -O1 -DACCESS_OFFSET=4 %s -o %t -mllvm -tsan-stc-preserve-uaf -mllvm -tsan-use-active-thread-count=false && %deflake %run %t | FileCheck %s
#include <stdlib.h>

#ifndef ACCESS_OFFSET
#define ACCESS_OFFSET 0
#endif

__attribute__((noinline)) void foo(void *mem) {
  free(mem);
}

__attribute__((noinline)) void baz(void *mem) {
  free(mem);
}

__attribute__((noinline)) void bar(void *mem) {
  *(long*)((char*)mem + ACCESS_OFFSET) = 42;
}

int main() {
  void *mem = malloc(100);
  baz(mem);
  mem = malloc(100);
  foo(mem);
  bar(mem);
  return 0;
}

// CHECK: WARNING: ThreadSanitizer: heap-use-after-free
// CHECK:   Write of size {{.*}} at {{.*}} by main thread:
// CHECK:     #0 bar
// CHECK:     #1 main
// CHECK:   Previous write of size 8 at {{.*}} by main thread:
// CHECK:     #0 free
// CHECK:     #{{1|2}} foo
// CHECK:     #{{2|3}} main
