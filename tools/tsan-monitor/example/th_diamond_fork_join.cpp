// Test: Diamond fork-join pattern with race
// Category: Thread Lifecycle
// Expectation: RACE
// Notes: Complex spawn/join dependencies lead to concurrent accesses
#include <pthread.h>

#include <cstdio>

static int data = 0;

static void* thread3(void*) {
  data = 333;
  printf("[T3] wrote data = 333\n");
  return nullptr;
}

static void* thread2(void*) {
  pthread_t t3;
  pthread_create(&t3, nullptr, thread3, nullptr);
  pthread_join(t3, nullptr);

  int v = data;
  printf("[T2] read data = %d\n", v);
  return nullptr;
}

static void* thread1(void*) {
  data = 111;
  printf("[T1] wrote data = 111\n");
  return nullptr;
}

int main() {
  printf("&data = %p\n", (void*)&data);

  data = 0;
  pthread_t t1, t2;

  // Fork: T0 spawns T1 and T2
  pthread_create(&t1, nullptr, thread1, nullptr);
  pthread_create(&t2, nullptr, thread2, nullptr);

  // Join: T0 waits for both
  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);

  // T0 reads after all joins
  int v = data;
  printf("[T0] read data = %d\n", v);

  return 0;
}
