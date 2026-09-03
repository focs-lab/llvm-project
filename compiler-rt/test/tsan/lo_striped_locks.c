// A striped lock array: two threads take different stripes and write the same
// global. Different stripes exclude nothing, so the race is real. Identifying a
// lock by its underlying object made every stripe "the lock locks[]" and let
// the write go uninstrumented. The race must be reported.

// RUN: %clang_tsan -O1 %s -o %t -mllvm -tsan-use-lock-ownership && %deflake %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <unistd.h>

pthread_mutex_t locks[2] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
int G;

void *stripe0(void *x) {
  pthread_mutex_lock(&locks[0]);
  G = 1;
  pthread_mutex_unlock(&locks[0]);
  return 0;
}

void *stripe1(void *x) {
  usleep(10000);
  pthread_mutex_lock(&locks[1]);
  G = 2;
  pthread_mutex_unlock(&locks[1]);
  return 0;
}

int main() {
  pthread_t t[2];
  pthread_create(&t[0], 0, stripe0, 0);
  pthread_create(&t[1], 0, stripe1, 0);
  pthread_join(t[0], 0);
  pthread_join(t[1], 0);
  return 0;
}

// CHECK: WARNING: ThreadSanitizer: data race
// CHECK: Location is global 'G'
