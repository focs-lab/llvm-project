// Two mutexes that live in the same struct are two mutexes. One thread writes
// G under S.a, the other under S.b: nothing excludes them and the race is
// real. Lock ownership used to identify a lock by the object it sits in, so
// both were "the lock S", the accesses looked consistently protected, and the
// instrumentation that reports this was dropped. The race must be reported.
//
// The flag is named on the RUN line so this is the same test whatever the
// harness injects.

// RUN: %clang_tsan -O1 %s -o %t -mllvm -tsan-use-lock-ownership && %deflake %run %t 2>&1 | FileCheck %s

#include <pthread.h>
#include <unistd.h>

struct { pthread_mutex_t a; pthread_mutex_t b; } S = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
int G;

void *under_a(void *x) {
  pthread_mutex_lock(&S.a);
  G = 1;
  pthread_mutex_unlock(&S.a);
  return 0;
}

void *under_b(void *x) {
  usleep(10000);
  pthread_mutex_lock(&S.b);
  G = 2;
  pthread_mutex_unlock(&S.b);
  return 0;
}

int main() {
  pthread_t t[2];
  pthread_create(&t[0], 0, under_a, 0);
  pthread_create(&t[1], 0, under_b, 0);
  pthread_join(t[0], 0);
  pthread_join(t[1], 0);
  return 0;
}

// CHECK: WARNING: ThreadSanitizer: data race
// CHECK: Location is global 'G'
