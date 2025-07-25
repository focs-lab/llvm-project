#include <pthread.h>
#include <stdio.h>

long global_data = 0;
pthread_mutex_t mutex1;

void* thread_func(void* arg) {
  // Perform 10 accesses in a loop with the same context
  for (int i = 0; i < 10; ++i) {
    pthread_mutex_lock(&mutex1);
    // The context here is always {mutex1}
    global_data++;
    pthread_mutex_unlock(&mutex1);
  }
  return NULL;
}

int main() {
  pthread_t t1;

  pthread_mutex_init(&mutex1, NULL);
  pthread_create(&t1, NULL, thread_func, NULL);
  pthread_join(t1, NULL);
  pthread_mutex_destroy(&mutex1);

  printf("--- Test: Intra-thread Redundancy ---\n");
  printf("Expected result in TSan stats:\n");
  printf("  - Total memory accesses: 10\n");
  printf("  - Accesses filtered: 9 (the first passes, the other 9 are redundant)\n");
  printf("  - Intra-thread redundancy: 9\n");
  return 0;
}