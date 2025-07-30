#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

long shared_data = 0;

void* thread_func_first(void* arg) {
  // This thread performs one access and leaves a "trace" in the filter
  // with an empty context {}.
  shared_data++;
  printf("First thread (TID probably %ld) finished.\n", (long)pthread_self());
  return NULL;
}

void* thread_func_second(void* arg) {
  // This, the SECOND, thread might get the same TID as the first one.
  // Its FIRST access must NOT be filtered.
  printf("Second thread (TID probably %ld) started.\n", (long)pthread_self());
  shared_data++;
  return NULL;
}

int main() {

  pthread_t t1, t2;

  // --- Phase 1: Create and finish the first thread ---
  pthread_create(&t1, NULL, thread_func_first, NULL);
  pthread_join(t1, NULL); // Wait for its complete termination

  // A small pause to give the system time to clean up thread resources
  sleep(1);

  // --- Phase 2: Create the second thread ---
  pthread_create(&t2, NULL, thread_func_second, NULL);
  pthread_join(t2, NULL);

  printf("\n--- Test: TID Reuse ---\n");
  printf("Expected result in TSan stats:\n");
  printf("  - Total memory accesses: 2\n");
  printf("  - Accesses filtered: 0\n");
  printf("Explanation:\n");
  printf("  - The access from the first thread is not redundant (it's the first).\n");
  printf("  - The access from the second thread is also not redundant because it's a\n");
  printf("    COMPLETELY NEW thread, even if its TID is the same. A correct\n");
  printf("    implementation with IsTidAlive should have cleaned up the 'dead' TID\n");
  printf("    of the first thread and passed the access from the second one.\n");
  printf("If 'Accesses filtered' > 0, you have a bug in your TID reuse handling.\n");

  return 0;
}