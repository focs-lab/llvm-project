// Test: Atomic exchange (RMW)
// Category: Atomics
// Expectation: NO_RACE
// Notes: Atomic RMW operations are race-free

#include <atomic>
#include <iostream>
#include <thread>

namespace {
std::atomic<int> token{0};
int shared_data = 0;

void thread1() {
  shared_data = 100;
  int old = token.exchange(1, std::memory_order_release);
  std::cout << "Thread 1 old token: " << old << "\n";
}

void thread2() {
  int val;
  while ((val = token.exchange(2, std::memory_order_acquire)) == 0) {
    // std::this_thread::yield();
  }
  // Safe: exchange with acquire sees release
  std::cout << "Thread 2 saw token=" << val << ", data=" << shared_data << "\n";
}
}  // namespace

int main() {
  std::thread t1(thread1);
  std::thread t2(thread2);
  t1.join();
  t2.join();
  return 0;
}
