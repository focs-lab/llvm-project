// Test: Race on shared counter without synchronization
// Category: Basic Race
// Expectation: RACE
// Notes: Non-atomic increments across threads
#include <pthread.h>

int counter1 = 0;
int counter2 = 0;
int counter3 = 0;

static void* increment_all(void*) {
  for (int i = 0; i < 1000; i++) {
    // Race: Read-modify-write of all three counters are not atomic
    counter1 = counter1 + 1;
    counter2 = counter2 + 1;
    counter3 = counter3 + 1;
  }
  return nullptr;
}

static void* decrement_all(void*) {
  for (int i = 0; i < 1000; i++) {
    // Race: Read-modify-write of all three counters are not atomic
    counter1 = counter1 - 1;
    counter2 = counter2 - 1;
    counter3 = counter3 - 1;
  }
  return nullptr;
}

int main() {
  pthread_t t1, t2, t3, t4;

  // 4 threads modify counters simultaneously
  pthread_create(&t1, nullptr, increment_all, nullptr);
  pthread_create(&t2, nullptr, increment_all, nullptr);
  pthread_create(&t3, nullptr, decrement_all, nullptr);
  pthread_create(&t4, nullptr, decrement_all, nullptr);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  pthread_join(t3, nullptr);
  pthread_join(t4, nullptr);

  // Expected: All counters should be 0
  // Actual: Due to race, results are unpredictable
  return 0;
}
