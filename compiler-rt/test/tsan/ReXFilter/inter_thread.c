#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define NUM_THREADS 4

long global_data = 0;
pthread_mutex_t mutex1;

void* thread_func(void* arg) {
  // All threads execute the exact same code
  pthread_mutex_lock(&mutex1);
  global_data++;
  pthread_mutex_unlock(&mutex1);
  return NULL;
}

int main() {
  pthread_t threads[NUM_THREADS];
  pthread_mutex_init(&mutex1, NULL);

  for (int i = 0; i < NUM_THREADS; ++i)
    pthread_create(&threads[i], NULL, thread_func, NULL);

  for (int i = 0; i < NUM_THREADS; ++i)
    pthread_join(threads[i], NULL);

  pthread_mutex_destroy(&mutex1);

  printf("--- Test: Inter-thread Redundancy ---\n");
  printf("Expected result in TSan stats:\n");
  printf("  - Total memory accesses: 4\n");
  printf("  - Accesses filtered: 2 (the first two pass, the 3rd and 4th are filtered)\n");
  printf("  - Inter-thread redundancy: 2\n");
  return 0;
}