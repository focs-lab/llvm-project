// Test: Broken double-checked locking
// Category: Atomics
// Expectation: MAYBE_RACE
// Notes: Uses relaxed atomics; may fail to synchronize initialization

#include <atomic>
#include <iostream>
#include <thread>

namespace {
std::atomic<int*> instance{nullptr};

int* get_instance_broken() {
  int* ptr =
      instance.load(std::memory_order_relaxed);  // BUG: should be acquire
  if (ptr == nullptr) {
    ptr = new int(42);
    instance.store(ptr, std::memory_order_relaxed);  // BUG: should be release
  }
  return ptr;
}

void thread_func() {
  int* ptr = get_instance_broken();
  // RACE: might see uninitialized memory
  std::cout << "Got value: " << *ptr << "\n";
}
}  // namespace

int main() {
  std::thread t1(thread_func);
  std::thread t2(thread_func);
  t1.join();
  t2.join();
  int* ptr = instance.load();
  if (ptr) delete ptr;
  return 0;
}
