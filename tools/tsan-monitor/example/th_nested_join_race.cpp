// Test: Race in nested join scenario
// Category: Thread Lifecycle
// Expectation: RACE
// Notes: Parent joins child but grandchild still runs
#include <pthread.h>
#include <unistd.h>

#include <cstdio>

static int data = 0;

static void* thread2(void*) {
  usleep(50000);  // T2 delays 50ms
  data = 222;     // T2 write (should race with T0 if join is broken)
  printf("[T2] wrote data = 222\n");
  return nullptr;
}

static void* thread1(void*) {
  pthread_t t2;
  pthread_create(&t2, nullptr, thread2, nullptr);

  // T1 exits WITHOUT joining T2 (detached grandchild)
  printf("[T1] exiting (T2 still running)\n");
  pthread_detach(t2);  // Detach so we don't have to join
  return nullptr;
}

int main() {
  printf("&data = %p\n", (void*)&data);

  pthread_t t1;
  pthread_create(&t1, nullptr, thread1, nullptr);

  pthread_join(t1, nullptr);  // T0 waits for T1

  // BUG: If join doesn't check descendants, this races with T2
  data = 100;  // T0 write (concurrent with T2 if join is broken)
  printf("[T0] wrote data = 100\n");

  usleep(100000);  // Wait for T2 to finish

  return 0;
}
