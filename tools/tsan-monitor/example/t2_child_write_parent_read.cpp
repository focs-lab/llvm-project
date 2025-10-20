// Child write – Join – Parent read (no race expected with HB)
#include <pthread.h>

#include <cstdio>

static int data = 0;

static void* child(void*) {
  data = 100;  // child write before join
  return nullptr;
}

int main() {
  pthread_t t;
  printf("&data = %p\n", (void*)&data);
  printf("&t = %p\n", (void*)&t);
  pthread_create(&t, nullptr, child, nullptr);
  pthread_join(t, nullptr);  // acquire HB from child
  int v = data;              // parent read after join
  (void)v;
  return 0;
}
