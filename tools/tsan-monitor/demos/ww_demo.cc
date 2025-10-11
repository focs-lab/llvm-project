#include <thread>

int x = 0;

void write_one() { x = 1; }
void write_two() { x = 2; }

int main() {
  std::thread t1(write_one);
  std::thread t2(write_two);
  t1.join();
  t2.join();
  return 0;
}
