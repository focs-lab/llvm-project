#include <pthread.h>

#define N 1

int x = 0;

void* thread_func(void* arg) {
  for (int i = 0; i < N; ++i)
    x++; // A lot of redundancy
  //x++;
  return NULL;
}

int main() {
  pthread_t t;
  pthread_create(&t, NULL, thread_func, NULL);
  for (int i = 0; i < N; ++i)
    x++;
  //x++;
  pthread_join(t, NULL);
  return 0;
}
