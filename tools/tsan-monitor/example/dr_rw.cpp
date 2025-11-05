// Test: Read-Write race on a shared int
// Category: Basic Race
// Expectation: RACE
// Notes: One thread writes while another reads without synchronization
#include <pthread.h>

int x = 0;
int r = 0;

static void* writer(void*) {
  x = 42;
  return nullptr;
}

static void* reader(void*) {
  r = x;
  return nullptr;
}

int main() {
  pthread_t t1;
  pthread_t t2;
  pthread_create(&t1, nullptr, writer, nullptr);
  pthread_create(&t2, nullptr, reader, nullptr);
  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  return 0;
}
