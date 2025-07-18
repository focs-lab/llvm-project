#include <pthread.h>

#define N 1000

int x = 0;
void* thread_func(void* arg) {
  for (int i = 0; i < N; ++i)
    x++; // A lot of redundancy
  return NULL;
}

int main() {
  pthread_t t;
  pthread_create(&t, NULL, thread_func, NULL);
  for (int i = 0; i < N; ++i)
    x++;
  pthread_join(t, NULL);
  return 0;
}