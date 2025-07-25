#include <pthread.h>
#include <stdio.h>

long global_data = 0;
pthread_mutex_t mutex1, mutex2;

// Thread 1: acquires L1, then L2
void* thread_func1(void* arg) {
  pthread_mutex_lock(&mutex1);
  pthread_mutex_lock(&mutex2);
  // Context here is {mutex1, mutex2}
  global_data++;
  pthread_mutex_unlock(&mutex2);
  pthread_mutex_unlock(&mutex1);
  return NULL;
}

// Thread 2: acquires L2, then L1
void* thread_func2(void* arg) {
  pthread_mutex_lock(&mutex2);
  pthread_mutex_lock(&mutex1);
  // Context here is also {mutex1, mutex2}
  global_data++;
  pthread_mutex_unlock(&mutex1);
  pthread_mutex_unlock(&mutex2);
  return NULL;
}

int main() {
  pthread_t t1, t2, t3;
  pthread_mutex_init(&mutex1, NULL);
  pthread_mutex_init(&mutex2, NULL);

  // Launch three threads with "equivalent" contexts
  pthread_create(&t1, NULL, thread_func1, NULL);
  pthread_create(&t2, NULL, thread_func2, NULL);
  pthread_create(&t3, NULL, thread_func1, NULL);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);
  pthread_join(t3, NULL);

  pthread_mutex_destroy(&mutex1);
  pthread_mutex_destroy(&mutex2);

  printf("--- Test: LockSet Canonization (Lock Order) ---\n");
  printf("Expected result in TSan stats:\n");
  printf("  - Total memory accesses: 3\n");
  printf("  - Accesses filtered: 1 (the first two pass, the 3rd is filtered)\n");
  printf("  - Inter-thread redundancy: 1\n");
  return 0;
}