// Test: Multiple threads acquire the same mutex
// Category: Mutex
// Expectation: NO_RACE
// Notes: Mutual exclusion serializes critical sections

#include <pthread.h>

#include <iostream>

namespace {
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
int shared_counter = 0;

struct ThreadArg {
  int tid;
};

void* increment_many(void* arg) {
  int tid = static_cast<ThreadArg*>(arg)->tid;
  for (int i = 0; i < 100; ++i) {
    pthread_mutex_lock(&mtx);
    ++shared_counter;
    pthread_mutex_unlock(&mtx);
  }
  std::cout << "Thread " << tid << " done\n";
  return nullptr;
}
}  // namespace

int main() {
  const int num_threads = 4;
  pthread_t threads[num_threads];
  ThreadArg args[num_threads];

  for (int i = 0; i < num_threads; ++i) {
    args[i].tid = i;
    pthread_create(&threads[i], nullptr, increment_many, &args[i]);
  }
  for (int i = 0; i < num_threads; ++i) {
    pthread_join(threads[i], nullptr);
  }
  std::cout << "Final counter: " << shared_counter << "\n";
  pthread_mutex_destroy(&mtx);
  return 0;
}
