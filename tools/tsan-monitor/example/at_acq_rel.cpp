// Test: Acquire-Release ordering (acq_rel)
// Category: Atomics
// Expectation: NO_RACE
// Notes: Bidirectional synchronization with acq_rel semantics

#include <atomic>
#include <iostream>
#include <thread>

namespace {
std::atomic<int> sync{0};
int data1 = 0;
int data2 = 0;

void producer() {
  data1 = 100;
  // Release: makes data1 visible to consumer
  sync.store(1, std::memory_order_release);
}

void middle() {
  int expected = 1;
  while (!sync.compare_exchange_strong(expected, 2, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    expected = 1;
    // std::this_thread::yield();
  }
  // Acquire: sees data1 from producer
  // Release: makes data2 visible to consumer
  std::cout << "Middle saw data1: " << data1 << "\n";
  data2 = 200;
}

void consumer() {
  while (sync.load(std::memory_order_acquire) != 2) {
    // std::this_thread::yield();
  }
  // Acquire: sees both data1 and data2
  std::cout << "Consumer saw data1=" << data1 << ", data2=" << data2 << "\n";
}
}  // namespace

int main() {
  std::thread t1(producer);
  std::thread t2(middle);
  std::thread t3(consumer);
  t1.join();
  t2.join();
  t3.join();
  return 0;
}
