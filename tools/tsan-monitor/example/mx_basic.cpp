// Test: Mutex lock/unlock around shared access
// Category: Mutex
// Expectation: NO_RACE
// Notes: Critical section protected by a single mutex
#include <pthread.h>
#include <unistd.h>

#include <cstdio>
#include <iostream>

namespace {
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
int shared_counter = 0;

void* increment(void*) {
  pthread_mutex_lock(&mtx);
  ++shared_counter;
  pthread_mutex_unlock(&mtx);
  return nullptr;
}

void* read_once(void*) {
  pthread_mutex_lock(&mtx);
  std::cout << "read once value=" << shared_counter << "\n";
  pthread_mutex_unlock(&mtx);
  return nullptr;
}
}  // namespace

int main() {
  pthread_t t1, t2;
  pthread_create(&t1, nullptr, increment, nullptr);
  pthread_create(&t2, nullptr, read_once, nullptr);

  printf("&mtx = %p\n", (void*)&mtx);
  printf("&shared_counter = %p\n", (void*)&shared_counter);
  printf("&t1 = %p\n", (void*)&t1);
  printf("&t2 = %p\n", (void*)&t2);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);

  // pthread_mutex_lock(&mtx);
  // std::cout << "final counter=" << shared_counter << "\n";
  // pthread_mutex_unlock(&mtx);

  pthread_mutex_destroy(&mtx);
  return 0;
}
