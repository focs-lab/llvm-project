// Test: Two atomic writes to the same location
// Category: Atomics
// Expectation: NO_RACE
// Notes: Atomic writes do not constitute a data race

#include <atomic>
#include <iostream>
#include <thread>

namespace {
std::atomic<int> value{0};

void writer1() { value.store(100, std::memory_order_relaxed); }

void writer2() { value.store(200, std::memory_order_relaxed); }
}  // namespace

int main() {
  std::thread t1(writer1);
  std::thread t2(writer2);
  t1.join();
  t2.join();
  // No race on atomic variable itself
  // Final value is either 100 or 200
  std::cout << "Final value: " << value.load() << "\n";
  return 0;
}
