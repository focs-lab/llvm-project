// Test: Acquire-release synchronization with atomics
// Category: Atomics
// Expectation: NO_RACE
// Notes: Release by producer and acquire by consumer establish HB
#include <atomic>
#include <iostream>
#include <thread>

namespace {
std::atomic<int> ready{0};
int shared_value = 0;

void producer() {
  shared_value = 42;
  ready.store(1, std::memory_order_release);
}

void consumer() {
  while (ready.load(std::memory_order_acquire) != 1) {
    // std::this_thread::yield();
  }
  // Might race, might not race: could be false negative
  // auto x = ready.load(std::memory_order_acquire);
  // std::cout << "x = " << x << std::endl;
  std::cout << "consumer observed value=" << shared_value << "\n";
}
}  // namespace

int main() {
  std::thread t1(producer);
  std::thread t2(consumer);
  printf("&ready = %p\n", (void*)&ready);
  printf("&shared_value = %p\n", (void*)&shared_value);
  printf("&t1 = %p\n", (void*)&t1);
  printf("&t2 = %p\n", (void*)&t2);
  t1.join();
  t2.join();
  return 0;
}
