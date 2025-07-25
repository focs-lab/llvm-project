#include <pthread.h>
#include <stdio.h>

long shared_data = 0;
pthread_mutex_t mutex1, mutex2;

// Thread 1 uses mutex1
void* thread_func1(void* arg) {
  pthread_mutex_lock(&mutex1);
  // Context is {mutex1}
  shared_data++;
  pthread_mutex_unlock(&mutex1);
  return NULL;
}

// Thread 2 uses mutex2
void* thread_func2(void* arg) {
  pthread_mutex_lock(&mutex2);
  // Context is {mutex2}
  shared_data++;
  pthread_mutex_unlock(&mutex2);
  return NULL;
}

int main() {
  pthread_t t1, t2;
  pthread_mutex_init(&mutex1, NULL);
  pthread_mutex_init(&mutex2, NULL);

  pthread_create(&t1, NULL, thread_func1, NULL);
  pthread_create(&t2, NULL, thread_func2, NULL);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  pthread_mutex_destroy(&mutex1);
  pthread_mutex_destroy(&mutex2);

  printf("--- Test: Race with Different Locks ---\n");
  printf("Expected result: TSan MUST report a data race on 'shared_data'.\n");
  printf("Filter behavior:\n");
  printf("  - The filter sees two different contexts: {mutex1} and {mutex2}.\n");
  printf("  - Since the contexts are different, both accesses are considered unique\n");
  printf("    and are NOT filtered. TSan sees both and reports the race.\n");
  return 0;
}