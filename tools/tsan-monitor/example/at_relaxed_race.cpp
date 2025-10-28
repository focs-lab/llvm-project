// Test: Relaxed atomics do not establish happens-before
// Category: Atomics
// Expectation: RACE
// Notes: Reader may observe stale data guarded only by relaxed flag

#include <atomic>
#include <iostream>
#include <thread>

namespace {
std::atomic<int> flag{0};
int shared_data = 0;

void writer() {
  shared_data = 42;
  // BUG: relaxed doesn't synchronize!
  flag.store(1, std::memory_order_relaxed);
}

void reader() {
  while (flag.load(std::memory_order_relaxed) != 1) {
    // std::this_thread::yield();
  }
  // RACE: No happens-before from writer to here
  std::cout << "value=" << shared_data << "\n";
}
}  // namespace

int main() {
  std::thread t1(writer);
  std::thread t2(reader);
  t1.join();
  t2.join();
  return 0;
}
