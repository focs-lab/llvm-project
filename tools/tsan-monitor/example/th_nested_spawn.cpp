// Test: Nested thread spawning with synchronization
// Category: Thread Lifecycle
// Expectation: NO_RACE
// Notes: Spawn chain ensures proper happens-before
#include <pthread.h>

#include <cstdio>

static int data = 0;

static void* thread3(void*) {
  data = 333;  // T3 write
  printf("[T3] wrote data = 333\n");
  return nullptr;
}

static void* thread2(void*) {
  data = 222;  // T2 write
  printf("[T2] wrote data = 222\n");

  pthread_t t3;
  pthread_create(&t3, nullptr, thread3, nullptr);
  pthread_join(t3, nullptr);

  int v = data;  // T2 read after join T3
  printf("[T2] read data = %d\n", v);
  return nullptr;
}

static void* thread1(void*) {
  data = 111;  // T1 write
  printf("[T1] wrote data = 111\n");

  pthread_t t2;
  pthread_create(&t2, nullptr, thread2, nullptr);
  pthread_join(t2, nullptr);

  int v = data;  // T1 read after join T2
  printf("[T1] read data = %d\n", v);
  return nullptr;
}

int main() {
  printf("&data = %p\n", (void*)&data);

  data = 0;  // T0 write
  printf("[T0] wrote data = 0\n");

  pthread_t t1;
  pthread_create(&t1, nullptr, thread1, nullptr);
  pthread_join(t1, nullptr);

  int v = data;  // T0 read after join T1
  printf("[T0] read data = %d\n", v);

  return 0;
}
