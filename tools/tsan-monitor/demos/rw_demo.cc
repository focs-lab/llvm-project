#include <thread>

int x = 0;
int r = 0;

void writer() { x = 42; }
void reader() { r = x; }

int main() {
  std::thread t1(writer);
  std::thread t2(reader);
  t1.join();
  t2.join();
  return r;
}
