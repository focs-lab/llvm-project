// Test: Mixing mutex and atomic synchronization correctly
// Category: Mutex
// Expectation: NO_RACE
// Notes: Atomics and locks combine to ensure happens-before

#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
std::mutex mtx;
std::atomic<bool> ready{false};
int data1 = 0;  // protected by mutex
int data2 = 0;  // protected by atomic

void thread1() {
  {
    std::lock_guard<std::mutex> lock(mtx);
    data1 = 100;
  }
  data2 = 200;
  ready.store(true, std::memory_order_release);
}

void thread2() {
  while (!ready.load(std::memory_order_acquire)) {
    // std::this_thread::yield();
  }
  // Safe: atomic provides HB for data2
  std::cout << "data2: " << data2 << "\n";

  std::lock_guard<std::mutex> lock(mtx);
  // Safe: mutex provides HB for data1
  std::cout << "data1: " << data1 << "\n";
}
}  // namespace

int main() {
  std::thread t1(thread1);
  std::thread t2(thread2);
  t1.join();
  t2.join();
  return 0;
}
