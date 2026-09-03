// Shadow-cell eviction counters. A granule keeps four shadow slots; the fifth
// distinct thread to touch it overwrites one. Under print_evictions the
// runtime reports how many overwrites happened and how many of them dropped
// an access of another thread that the evicting thread's clock did not cover
// -- the only evictions that can hide a later race. Here every write is
// ordered by the join in main, so the concurrent counts are zero and the
// total is not; the interceptors' atomic reads on the mutex word are what
// makes the unordered subset non-zero in lock-heavy code, and the "plain"
// subset excludes them. Nothing is printed by default.
//
// RUN: %clang_tsan -O1 %s -o %t
// RUN: %env_tsan_opts=print_evictions=1 %run %t 2>&1 | FileCheck %s
// RUN: %run %t 2>&1 | FileCheck %s --check-prefix=QUIET --allow-empty
// RUN: %env_tsan_opts=print_evictions=1:evict_watch=0x10+0x20 %run %t 2>&1 | FileCheck %s --check-prefix=WATCH
#include "test.h"

static char buf[8];

static void *w(void *a) {
  buf[(long)a % 8] = 1;
  return 0;
}

int main() {
  pthread_t t;
  for (long i = 0; i < 16; i++) {
    pthread_create(&t, 0, w, (void *)i);
    pthread_join(t, 0);
  }
  fprintf(stderr, "DONE %d\n", buf[0]);
  return 0;
}

// CHECK: ThreadSanitizer: shadow evictions total={{[1-9][0-9]*}} concurrent_foreign=0 concurrent_foreign_plain=0
// CHECK-NOT: WARNING: ThreadSanitizer
// QUIET-NOT: shadow evictions
// QUIET: DONE 1
// WATCH: shadow evictions total=
// WATCH-NEXT: ThreadSanitizer: evictions at 0x{{0*}}10: total=0 concurrent_foreign=0 concurrent_foreign_plain=0
// WATCH-NEXT: ThreadSanitizer: evictions at 0x{{0*}}20: total=0 concurrent_foreign=0 concurrent_foreign_plain=0
