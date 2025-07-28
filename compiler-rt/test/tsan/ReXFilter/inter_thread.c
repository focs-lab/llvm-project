#include <pthread.h>

#include <sanitizer/tsan_interface.h>
#include <stdbool.h>
extern volatile bool InterceptorEnabled;

#include <stdio.h>
#include <unistd.h>

#define NUM_THREADS 100

long global_data = 0;
pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
pthread_barrier_t barrier;

void* thread_func(void* arg) {
  // All threads execute the exact same code
  pthread_mutex_lock(&mutex1);

  global_data = 111;

  pthread_mutex_unlock(&mutex1);
  pthread_barrier_wait(&barrier);
  return NULL;
}

__attribute__((no_sanitize("thread")))
int main() {
  InterceptorEnabled = 0;
  printf("Address of global_data: %p\n", (void*)&global_data);

  pthread_t threads[NUM_THREADS];
  pthread_barrier_init(&barrier, NULL, NUM_THREADS);
  InterceptorEnabled = 1;

  for (int i = 0; i < NUM_THREADS; ++i)
    pthread_create(&threads[i], NULL, thread_func, NULL);

  for (int i = 0; i < NUM_THREADS; ++i)
    pthread_join(threads[i], NULL);

  InterceptorEnabled = 0;
  pthread_mutex_destroy(&mutex1);

  printf("--- Test: Inter-thread Redundancy ---\n");
  printf("Expected result in TSan stats:\n");
  printf("  - Total memory accesses: 4\n");
  printf("  - Accesses filtered: 2 (the first two pass, the 3rd and 4th are filtered)\n");
  printf("  - Inter-thread redundancy: 2\n");
  InterceptorEnabled = 1;

  return 0;
}