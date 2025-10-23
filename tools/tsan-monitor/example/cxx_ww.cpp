#include <iostream>
#include <thread>

int global_var = 0;

void write_func(int id) { global_var = id; }

int main() {
  std::cout << "&global_var = " << &global_var << std::endl;
  std::thread t1(write_func, 1);
  std::thread t2(write_func, 2);
  // global_var = 3; // Race
  t1.join();
  t2.join();
  global_var = 4;  // No race
  return 0;
}