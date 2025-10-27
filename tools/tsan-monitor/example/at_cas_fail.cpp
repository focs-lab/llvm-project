// Test: Compare-exchange failure path
// Category: Atomics
// Expectation: NO_RACE
// Notes: Failure uses acquire semantics and remains race-free

#include <atomic>
#include <iostream>
#include <thread>

namespace {
std::atomic<int> value{10};
int shared_data = 0;

void writer() {
  shared_data = 999;
  value.store(20, std::memory_order_release);
}

void reader() {
  int expected = 10;
  // This will fail because writer changes it to 20
  bool success = value.compare_exchange_strong(
      expected, 30,
      std::memory_order_release,  // success order (not used)
      std::memory_order_acquire   // failure order (used!)
  );

  if (!success) {
    std::cout << "CAS failed, expected was updated to: " << expected << "\n";
    // Safe to read: failure acquire synchronizes with release
    std::cout << "Observed shared_data: " << shared_data << "\n";
  }
}
}  // namespace

int main() {
  std::thread t1(writer);
  std::thread t2(reader);
  t1.join();
  t2.join();
  return 0;
}
