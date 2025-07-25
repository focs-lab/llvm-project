#include <pthread.h>
#include <stdio.h>

#define NUM_THREADS 4

long properly_locked_data = 0;
long racy_data = 0;
pthread_mutex_t mutex1;

void* thread_func(void* arg) {
  // 1. A properly locked access.
  // After the first two threads, these accesses should be filtered.
  pthread_mutex_lock(&mutex1);
  properly_locked_data++;
  pthread_mutex_unlock(&mutex1);

  // 2. An unprotected, racy access.
  // The filter must not prevent TSan from seeing this race.
  racy_data++;

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

  printf("--- Test: Race Hidden Among Redundant Accesses ---\n");
  printf("Expected result: TSan MUST report a data race on 'racy_data'.\n");
  printf("Filter behavior:\n");
  printf("  - Accesses to 'properly_locked_data': 2 will be filtered as redundant.\n");
  printf("  - Accesses to 'racy_data': The first two accesses (from T1, T2) have an\n");
  printf("    empty context and will pass through the filter. This is enough for TSan\n");
  printf("    to see the conflict and report the race.\n");
  printf("  - Total accesses: 8, Filtered: 4 (2 for each variable)\n");
  return 0;
}