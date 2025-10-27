// Test: Read-write lock synchronization
// Category: Mutex
// Expectation: NO_RACE
// Notes: Multiple readers and exclusive writer with proper locking

#include <pthread.h>
#include <stdio.h>

pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
int shared_value = 0;

void* reader(void* arg) {
  int id = *(int*)arg;
  pthread_mutex_lock(&mtx);
  printf("reader%d: shared_value=%d\n", id, shared_value);
  pthread_mutex_unlock(&mtx);
  return NULL;
}

void* writer(void* arg) {
  int new_value = *(int*)arg;
  pthread_mutex_lock(&mtx);
  shared_value = new_value;
  pthread_mutex_unlock(&mtx);
  return NULL;
}

int main() {
  pthread_t threads[5];
  int args[5];

  printf("&mtx = %p\n", (void*)&mtx);
  printf("&shared_value = %p\n", (void*)&shared_value);
  printf("&threads = %p\n", (void*)&threads);
  printf("&writer = %p\n", (void*)writer);
  printf("&reader = %p\n", (void*)reader);

  args[0] = 10;
  pthread_create(&threads[0], NULL, writer, &args[0]);

  args[1] = 1;
  pthread_create(&threads[1], NULL, reader, &args[1]);

  args[2] = 2;
  pthread_create(&threads[2], NULL, reader, &args[2]);

  args[3] = 11;
  pthread_create(&threads[3], NULL, writer, &args[3]);

  args[4] = 3;
  pthread_create(&threads[4], NULL, reader, &args[4]);

  for (int i = 0; i < 5; ++i) {
    pthread_join(threads[i], NULL);
  }

  pthread_mutex_destroy(&mtx);
  return 0;
}
