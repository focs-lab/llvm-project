#include <pthread.h>
#include <stdio.h>

long global_data1 = 0;
long global_data2 = 0;
pthread_mutex_t mutex1;

// This function has its own PC for the access
void helper_access() {
  pthread_mutex_lock(&mutex1);
  // Access #1: PC_helper, Addr1, Context {L1}
  global_data1++;
  pthread_mutex_unlock(&mutex1);
}

void* thread_func(void* arg) {
  // 1. First access is via the helper function
  helper_access();

  // 2. Second access is directly here (different PC)
  pthread_mutex_lock(&mutex1);
  // Access #2: PC_main, Addr1, Context {L1}
  global_data1++;
  pthread_mutex_unlock(&mutex1);

  // 3. Third access is to a different address
  pthread_mutex_lock(&mutex1);
  // Access #3: PC_main, Addr2, Context {L1}
  global_data2++;
  pthread_mutex_unlock(&mutex1);

  return NULL;
}

int main() {
  pthread_t t1;
  pthread_mutex_init(&mutex1, NULL);

  pthread_create(&t1, NULL, thread_func, NULL);
  pthread_join(t1, NULL);

  pthread_mutex_destroy(&mutex1);

  printf("--- Test: Different PCs and Addresses ---\n");
  printf("Expected result in TSan stats:\n");
  printf("  - Total memory accesses: 3\n");
  printf("  - Accesses filtered: 0 (All accesses are unique by PC or Addr)\n");
  return 0;
}