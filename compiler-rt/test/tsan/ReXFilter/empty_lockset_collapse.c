#include <pthread.h>
#include <stdio.h>

#define NUM_VARS 100
long separate_data[NUM_VARS];

void* thread_func(void* arg) {
  // This thread accesses 100 DIFFERENT variables.
  // All accesses occur with an empty context {}.
  // NONE of these accesses are redundant with each other,
  // as they are to different memory addresses.
  for (int i = 0; i < NUM_VARS; ++i) {
    separate_data[i]++;
  }
  return NULL;
}

__attribute__((no_sanitize("thread")))
int main() {
  pthread_t t1;
  pthread_create(&t1, NULL, thread_func, NULL);
  pthread_join(t1, NULL);

  printf("\n--- Test: Empty LockSet Collapse ---\n");
  printf("Expected result in TSan stats:\n");
  printf("  - Total memory accesses: 100\n");
  printf("  - Accesses filtered: 0\n");
  printf("Explanation:\n");
  printf("  - All 100 accesses have the same PC and the same context {}.\n");
  printf("  - BUT they are to DIFFERENT memory addresses (Addr).\n");
  printf("  - A correct PC -> Addr -> Trie hierarchy should create 100 different\n");
  printf("    Trie roots for each address, and no access should be filtered.\n");
  printf("If 'Accesses filtered' > 0, you have a bug in your filter hierarchy (you might\n");
  printf("    not be using Addr as part of the key).\n");

  return 0;
}