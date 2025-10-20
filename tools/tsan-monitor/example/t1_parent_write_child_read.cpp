// Parent write – Spawn – Child read (no race expected with HB)
#include <pthread.h>

#include <cstdio>

static int data = 0;

static void* child(void*) {
  int v = data;  // read after spawn HB; should not race with parent write
  (void)v;
  return nullptr;
}

int main() {
  data = 42;  // parent write before spawn
  printf("&data = %p\n", (void*)&data);
  pthread_t t;
  printf("&t = %p\n", (void*)&t);
  pthread_create(&t, nullptr, child, nullptr);  // read t
  pthread_join(t, nullptr);
  return 0;
}
