#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

pthread_mutex_t mutex1;

void use_memory(long* ptr) {
  pthread_mutex_lock(&mutex1);
  // Access with context {L1}
  (*ptr)++;
  pthread_mutex_unlock(&mutex1);
}

int main() {
  printf("--- Test: Memory Reuse ---\n");

  // Phase 1: Use memory for the first time
  long* ptr1 = (long*)malloc(sizeof(long));
  printf("Phase 1: Allocated memory at %p\n", (void*)ptr1);
  use_memory(ptr1); // First access, should not be filtered
  use_memory(ptr1); // Second access, SHOULD be filtered (intra-thread redundancy)
  free(ptr1);
  printf("Phase 1: Freed memory.\n");

  // Phase 2: Allocate memory again. The allocator will likely return the same address.
  long* ptr2 = (long*)malloc(sizeof(long));
  printf("Phase 2: Allocated memory at %p\n", (void*)ptr2);

  // THIS ACCESS IS THE MOST IMPORTANT.
  // This is the first access to a NEW object, even though at the old address.
  // It SHOULD NOT be filtered.
  use_memory(ptr2);
  free(ptr2);

  printf("\nExpected result in TSan statistics:\n");
  printf("  - Total memory accesses: 3\n");
  printf("  - Accesses filtered: 1 (only the second access from Phase 1)\n");
  printf("Explanation:\n");
  printf("  - If the filter doesn't clear its history on free(), the access in Phase 2\n");
  printf("    will be incorrectly filtered. If 'Accesses filtered' > 1, you have this issue.\n");

  return 0;
}