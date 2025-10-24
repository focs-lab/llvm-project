// Test: Mutex race - one thread forgets to lock
// Expected: RACE detected (Write without lock vs Write with lock)

#include <iostream>
#include <mutex>
#include <thread>

namespace {
std::mutex mtx;
int shared_data = 0;

void thread_with_lock() {
  std::lock_guard<std::mutex> lock(mtx);
  shared_data = 100;
}

void thread_without_lock() {
  // BUG: Missing lock!
  shared_data = 200;
}
}  // namespace

int main() {
  std::thread t1(thread_with_lock);
  std::thread t2(thread_without_lock);
  t1.join();
  t2.join();
  std::cout << "Final value: " << shared_data << "\n";
  return 0;
}
