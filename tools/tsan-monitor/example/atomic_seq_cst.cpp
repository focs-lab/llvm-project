// Test: Sequential consistency (seq_cst) ordering
// Expected: NO RACE (seq_cst provides full synchronization)

#include <atomic>
#include <iostream>
#include <thread>

namespace {
std::atomic<int> x{0};
std::atomic<int> y{0};
int r1, r2;

void thread1() {
  x.store(1, std::memory_order_seq_cst);
  r1 = y.load(std::memory_order_seq_cst);
}

void thread2() {
  y.store(1, std::memory_order_seq_cst);
  r2 = x.load(std::memory_order_seq_cst);
}
}  // namespace

int main() {
  std::thread t1(thread1);
  std::thread t2(thread2);
  t1.join();
  t2.join();
  // seq_cst guarantees: r1 == 0 && r2 == 0 is impossible
  std::cout << "r1=" << r1 << ", r2=" << r2 << "\n";
  return 0;
}
