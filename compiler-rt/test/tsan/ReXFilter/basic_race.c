#include <pthread.h>
#include <stdio.h>

long racy_data = 0;

void* thread_func(void* arg) {
  // This access is unprotected.
  // The lockset context is empty {}.
  racy_data++;
  return NULL;
}

int main() {
  pthread_t t1, t2;

  pthread_create(&t1, NULL, thread_func, NULL);
  pthread_create(&t2, NULL, thread_func, NULL);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  printf("--- Test: Basic Data Race ---\n");
  printf("Expected result: TSan MUST report a data race on 'racy_data'.\n");
  printf("Filter behavior:\n");
  printf("  - The filter should NOT filter the accesses from the first two threads\n");
  printf("    because their context is the same ({}), but they are from different threads.\n");
  printf("  - This allows TSan to see both conflicting accesses and report the race.\n");
  return 0;
}