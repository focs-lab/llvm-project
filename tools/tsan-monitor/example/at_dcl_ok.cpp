// Test: Correct double-checked locking pattern
// Category: Atomics
// Expectation: NO_RACE
// Notes: Uses acquire/release atomics and mutex to synchronize init

#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
std::atomic<int*> instance{nullptr};
std::mutex init_mutex;

int* get_instance() {
  int* ptr = instance.load(std::memory_order_acquire);
  if (ptr == nullptr) {
    std::lock_guard<std::mutex> lock(init_mutex);
    ptr = instance.load(std::memory_order_relaxed);
    if (ptr == nullptr) {
      ptr = new int(42);
      instance.store(ptr, std::memory_order_release);
    }
  }
  return ptr;
}

void thread_func() {
  int* ptr = get_instance();
  std::cout << "Got instance: " << *ptr << "\n";
}
}  // namespace

int main() {
  std::thread t1(thread_func);
  std::thread t2(thread_func);
  std::thread t3(thread_func);
  t1.join();
  t2.join();
  t3.join();
  delete instance.load();
  return 0;
}
