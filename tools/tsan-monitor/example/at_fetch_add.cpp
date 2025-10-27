// Test: Atomic fetch-add (RMW)
// Category: Atomics
// Expectation: NO_RACE
// Notes: Atomic read-modify-write is race-free

#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

namespace {
std::atomic<int> counter{0};

void increment() {
  for (int i = 0; i < 1000; ++i) {
    counter.fetch_add(1, std::memory_order_relaxed);
  }
}
}  // namespace

int main() {
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back(increment);
  }
  for (auto& t : threads) {
    t.join();
  }
  std::cout << "Final counter: " << counter.load() << "\n";
  return 0;
}
