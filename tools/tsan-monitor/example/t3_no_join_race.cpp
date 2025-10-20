// No join – concurrent writes (race expected)
#include <pthread.h>

#include <cstdio>

static int data = 0;

static void* child(void*) {
  data = 42;  // child write
  return nullptr;
}

int main() {
  pthread_t t;
  pthread_create(&t, nullptr, child, nullptr);
  data = 100;  // parent write without HB before join
  pthread_join(t, nullptr);
  return 0;
}
