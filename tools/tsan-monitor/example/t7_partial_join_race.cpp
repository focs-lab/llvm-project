// Partial join: T0 spawns T1, T2 but only joins T1 (race with T2 expected)
#include <pthread.h>
#include <unistd.h>

#include <cstdio>

static int data = 0;

static void* thread1(void*) {
  data = 111;
  printf("[T1] wrote data = 111\n");
  return nullptr;
}

static void* thread2(void*) {
  usleep(10000);  // T2 delays slightly
  data = 222;     // T2 write (concurrent with T0)
  printf("[T2] wrote data = 222\n");
  return nullptr;
}

int main() {
  printf("&data = %p\n", (void*)&data);

  pthread_t t1, t2;
  pthread_create(&t1, nullptr, thread1, nullptr);
  pthread_create(&t2, nullptr, thread2, nullptr);

  pthread_join(t1, nullptr);  // Only join T1

  // Race: T0 writes while T2 might still be writing
  data = 100;
  printf("[T0] wrote data = 100\n");

  pthread_join(t2, nullptr);  // Clean up T2

  return 0;
}
