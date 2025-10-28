// Test: Successful compare-and-swap synchronization
// Category: Atomics
// Expectation: NO_RACE
// Notes: CAS establishes happens-before on success
#include <atomic>
#include <iostream>
#include <thread>

namespace {
std::atomic<int> state{0};
int shared_data = 0;

void writer() {
  shared_data = 2025;
  int expected = 0;
  while (!state.compare_exchange_strong(expected, 1, std::memory_order_release,
                                        std::memory_order_relaxed)) {
    expected = 0;
  }
}

void reader() {
  int expected = 1;
  while (!state.compare_exchange_strong(expected, 2, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
    expected = 1;
    // std::this_thread::yield();
  }
  std::cout << "reader saw shared_data=" << shared_data << "\n";
}
}  // namespace

int main() {
  std::thread t1(writer);
  std::thread t2(reader);
  t1.join();
  t2.join();
  return 0;
}
