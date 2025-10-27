// Test: Independent mutexes protect different variables
// Category: Mutex
// Expectation: NO_RACE
// Notes: Each variable guarded by its own lock

#include <pthread.h>

#include <iostream>

namespace {
pthread_mutex_t mtx1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mtx2 = PTHREAD_MUTEX_INITIALIZER;
int data1 = 0;
int data2 = 0;

void* thread1(void*) {
  pthread_mutex_lock(&mtx1);
  data1 = 100;
  pthread_mutex_unlock(&mtx1);

  pthread_mutex_lock(&mtx2);
  data2 = 200;
  pthread_mutex_unlock(&mtx2);

  return nullptr;
}

void* thread2(void*) {
  pthread_mutex_lock(&mtx1);
  data1 = 300;
  pthread_mutex_unlock(&mtx1);

  pthread_mutex_lock(&mtx2);
  data2 = 400;
  pthread_mutex_unlock(&mtx2);

  return nullptr;
}
}  // namespace

int main() {
  pthread_t t1, t2;
  pthread_create(&t1, nullptr, thread1, nullptr);
  pthread_create(&t2, nullptr, thread2, nullptr);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);

  std::cout << "data1=" << data1 << ", data2=" << data2 << "\n";

  pthread_mutex_destroy(&mtx1);
  pthread_mutex_destroy(&mtx2);
  return 0;
}
