// Multiple children: T0 spawns T1, T2, T3 and joins all (no race expected)
#include <pthread.h>

#include <cstdio>

static int data1 = 0;
static int data2 = 0;
static int data3 = 0;

static void* thread1(void*) {
  data1 = 111;
  printf("[T1] wrote data1 = 111\n");
  return nullptr;
}

static void* thread2(void*) {
  data2 = 222;
  printf("[T2] wrote data2 = 222\n");
  return nullptr;
}

static void* thread3(void*) {
  data3 = 333;
  printf("[T3] wrote data3 = 333\n");
  return nullptr;
}

int main() {
  printf("&data1 = %p, &data2 = %p, &data3 = %p\n", (void*)&data1,
         (void*)&data2, (void*)&data3);

  pthread_t t1, t2, t3;

  // Spawn all children
  pthread_create(&t1, nullptr, thread1, nullptr);
  pthread_create(&t2, nullptr, thread2, nullptr);
  pthread_create(&t3, nullptr, thread3, nullptr);

  // Join all children (establishes HB edges)
  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  pthread_join(t3, nullptr);

  // Parent reads after all joins - no race
  int v1 = data1;
  int v2 = data2;
  int v3 = data3;
  printf("[T0] read data1=%d, data2=%d, data3=%d\n", v1, v2, v3);

  return 0;
}
