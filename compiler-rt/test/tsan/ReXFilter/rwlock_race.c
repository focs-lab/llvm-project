#include <pthread.h>
#include <stdio.h>

long shared_data = 0;
pthread_rwlock_t rwlock;

// Reader thread: acquires read lock
void* reader_thread(void* arg) {
  pthread_rwlock_rdlock(&rwlock);
  // Context {rwlock_read}
  long local = shared_data;
  pthread_rwlock_unlock(&rwlock);
  return (void*)local;
}

// Writer thread: acquires write lock
void* writer_thread(void* arg) {
  pthread_rwlock_wrlock(&rwlock);
  // Context {rwlock_write}. RACE!
  shared_data++;
  pthread_rwlock_unlock(&rwlock);
  return NULL;
}

int main() {
  pthread_t reader1, writer;
  pthread_rwlock_init(&rwlock, NULL);

  // Start reader and writer simultaneously.
  // If scheduler runs them in wrong order,
  // TSan should detect the race.
  pthread_create(&reader1, NULL, reader_thread, NULL);
  pthread_create(&writer, NULL, writer_thread, NULL);

  pthread_join(reader1, NULL);
  pthread_join(writer, NULL);
    
  pthread_rwlock_destroy(&rwlock);

  printf("\n--- Test: Race with Read-Write Lock ---\n");
  printf("Expected result: TSan MUST report a race.\n");
  printf("Filter behavior:\n");
  printf("  - Filter must distinguish context from rdlock and wrlock.\n");
  printf("  - If it doesn't distinguish them, it may erroneously filter writer's access\n");
  printf("    as redundant relative to reader's (or vice versa).\n");
    
  return 0;
}