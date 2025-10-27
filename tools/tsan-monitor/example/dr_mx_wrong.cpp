// Test: Different locks used for the same data
// Category: Mutex
// Expectation: RACE
// Notes: Distinct mutexes do not synchronize each other

#include <iostream>
#include <mutex>
#include <thread>

namespace {
std::mutex mtx1;
std::mutex mtx2;
int shared_data = 0;

void thread1() {
  std::lock_guard<std::mutex> lock(mtx1);
  shared_data = 111;
}

void thread2() {
  // BUG: Using wrong mutex!
  std::lock_guard<std::mutex> lock(mtx2);
  shared_data = 222;
}
}  // namespace

int main() {
  std::thread t1(thread1);
  std::thread t2(thread2);
  t1.join();
  t2.join();
  std::cout << "Final value: " << shared_data << "\n";
  return 0;
}
