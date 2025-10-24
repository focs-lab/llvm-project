// Test: Multiple threads acquiring the same lock sequentially
// Expected: NO RACE (lock provides total order)

#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::mutex mtx;
int shared_counter = 0;

void increment_many(int tid) {
  for (int i = 0; i < 100; ++i) {
    std::lock_guard<std::mutex> lock(mtx);
    ++shared_counter;
  }
  std::cout << "Thread " << tid << " done\n";
}
}  // namespace

int main() {
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back(increment_many, i);
  }
  for (auto& t : threads) {
    t.join();
  }
  std::cout << "Final counter: " << shared_counter << "\n";
  return 0;
}
