#include <pthread.h>

int x = 0;

static void *write_one(void *) {
  x = 1;
  return nullptr;
}

static void *write_two(void *) {
  x = 2;
  return nullptr;
}

int main() {
  pthread_t t1;
  pthread_t t2;
  pthread_create(&t1, nullptr, write_one, nullptr);
  pthread_create(&t2, nullptr, write_two, nullptr);
  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  return 0;
}
