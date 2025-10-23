// Test: Lock ordering matters for race detection
// Expected: NO RACE (consistent lock ordering prevents race)

#include <iostream>
#include <mutex>
#include <thread>

namespace {
std::mutex mtx1;
std::mutex mtx2;
int shared_data = 0;

void thread1() {
  std::lock_guard<std::mutex> lock1(mtx1);
  std::lock_guard<std::mutex> lock2(mtx2);
  shared_data = 111;
}

void thread2() {
  std::lock_guard<std::mutex> lock1(mtx1);
  std::lock_guard<std::mutex> lock2(mtx2);
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
