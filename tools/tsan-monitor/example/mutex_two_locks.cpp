// Test: Two separate mutexes protecting different data
// Expected: NO RACE (each data protected by its own mutex)

#include <iostream>
#include <mutex>
#include <thread>

namespace {
std::mutex mtx1;
std::mutex mtx2;
int data1 = 0;
int data2 = 0;

void thread1() {
  {
    std::lock_guard<std::mutex> lock(mtx1);
    data1 = 100;
  }
  {
    std::lock_guard<std::mutex> lock(mtx2);
    data2 = 200;
  }
}

void thread2() {
  {
    std::lock_guard<std::mutex> lock(mtx1);
    data1 = 300;
  }
  {
    std::lock_guard<std::mutex> lock(mtx2);
    data2 = 400;
  }
}
}  // namespace

int main() {
  std::thread t1(thread1);
  std::thread t2(thread2);
  t1.join();
  t2.join();
  std::cout << "data1=" << data1 << ", data2=" << data2 << "\n";
  return 0;
}
