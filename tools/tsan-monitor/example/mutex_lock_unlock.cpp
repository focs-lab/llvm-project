#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>

namespace {
std::mutex mtx;
int shared_counter = 0;

void increment() {
  std::lock_guard<std::mutex> lock(mtx);
  ++shared_counter;
}

void read_once() {
  std::lock_guard<std::mutex> lock(mtx);
  std::cout << "read once value=" << shared_counter << "\n";
}
}  // namespace

int main() {
  std::thread t1(increment);
  std::thread t2(read_once);
  printf("&mtx = %p\n", (void*)&mtx);
  printf("&shared_counter = %p\n", (void*)&shared_counter);
  printf("&t1 = %p\n", (void*)&t1);
  printf("&t2 = %p\n", (void*)&t2);
  t1.join();
  t2.join();
  // std::lock_guard<std::mutex> final_lock(mtx);
  // std::cout << "final counter=" << shared_counter << "\n";
  return 0;
}
