// Test: Consistent lock acquisition order
// Category: Mutex
// Expectation: NO_RACE
// Notes: Avoids deadlock and data races via ordering

#include <pthread.h>

#include <iostream>

namespace {
pthread_mutex_t mtx1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mtx2 = PTHREAD_MUTEX_INITIALIZER;
int shared_data = 0;

void* thread1(void*) {
  pthread_mutex_lock(&mtx1);
  pthread_mutex_lock(&mtx2);
  shared_data = 111;
  pthread_mutex_unlock(&mtx2);
  pthread_mutex_unlock(&mtx1);
  return nullptr;
}

void* thread2(void*) {
  pthread_mutex_lock(&mtx1);
  pthread_mutex_lock(&mtx2);
  shared_data = 222;
  pthread_mutex_unlock(&mtx2);
  pthread_mutex_unlock(&mtx1);
  return nullptr;
}
}  // namespace

int main() {
  pthread_t t1, t2;
  pthread_create(&t1, nullptr, thread1, nullptr);
  pthread_create(&t2, nullptr, thread2, nullptr);
  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  std::cout << "Final value: " << shared_data << "\n";
  pthread_mutex_destroy(&mtx1);
  pthread_mutex_destroy(&mtx2);
  return 0;
}
