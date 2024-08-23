// RUN: %clangxx_psan -O1 %s -o %t && %deflake %run %t | FileCheck %s
#include <pthread.h>
#include <unistd.h>

int main() {
  pthread_mutex_t m;
  pthread_mutex_init(&m, 0);
  pthread_mutex_lock(&m);
  pthread_mutex_destroy(&m);
  return 0;
}

// CHECK: WARNING: PredictiveSanitizer: destroy of a locked mutex
// CHECK:     #0 pthread_mutex_destroy
// CHECK:     #1 main
// CHECK:   and:
// CHECK:     #0 pthread_mutex_lock
// CHECK:     #1 main
// CHECK:   Mutex {{.*}} created at:
// CHECK:     #0 pthread_mutex_init
// CHECK:     #1 main
// CHECK: SUMMARY: PredictiveSanitizer: destroy of a locked mutex{{.*}}main
