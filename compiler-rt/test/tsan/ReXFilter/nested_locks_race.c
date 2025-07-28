#include <pthread.h>
#include <stdio.h>

#define NITER 100

long shared_data = 0;
pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex2 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex3 = PTHREAD_MUTEX_INITIALIZER;
pthread_barrier_t barrier;

// Thread 1: Acquires L1, then L2.
void* thread_func1(void* arg) {
      pthread_mutex_lock(&mutex1);
          // Access #1. Context: {L1}
          //shared_data++;

          pthread_mutex_lock(&mutex2);
              // Access #2. Context: {L1, L2}
          //for (int i = 0; i < NITER; ++i) {
          shared_data = 111;
          //}
          pthread_mutex_unlock(&mutex2);

      pthread_mutex_unlock(&mutex1);
      pthread_barrier_wait(&barrier);
    return NULL;
}

// Thread 2: Acquires L1, then L3. This creates a RACE with Thread 1.
void* thread_func2(void* arg) {
      pthread_mutex_lock(&mutex1);
          // Access #3. Context: {L1}. Might be redundant with Access #1.
          //shared_data++;

          pthread_mutex_lock(&mutex3);
          //for (int i = 0; i < NITER; ++i) {
              shared_data = 222;
          //}
          pthread_mutex_unlock(&mutex3);

      pthread_mutex_unlock(&mutex1);
      pthread_barrier_wait(&barrier);
    return NULL;
}

__attribute__((no_sanitize("thread")))
int main() {
    fprintf(stderr, "Address of shared_data: %p\n", (void*)&shared_data);

    pthread_t t1, t2;
    pthread_barrier_init(&barrier, NULL, 2);
  /*
    pthread_mutex_init(&mutex1, NULL);
    pthread_mutex_init(&mutex2, NULL);
    pthread_mutex_init(&mutex3, NULL);
  */

    for (int i = 0; i < NITER; ++i) {
      pthread_create(&t1, NULL, thread_func1, NULL);
      pthread_create(&t2, NULL, thread_func2, NULL);

      pthread_join(t1, NULL);
      pthread_join(t2, NULL);
    }

    pthread_mutex_destroy(&mutex1);
    pthread_mutex_destroy(&mutex2);
    pthread_mutex_destroy(&mutex3);
    pthread_barrier_destroy(&barrier);

    printf("--- Test: Nested Locks and Race at Different Depths ---\n");
    printf("Expected result: TSan MUST report a data race.\n");
    printf("Filter behavior:\n");
    printf("  - Access #1 (T1, {L1}) and #3 (T2, {L1}) have the same context.\n");
    printf("    Both will pass since they are from different threads.\n");
    printf("  - Access #2 (T1, {L1, L2}) and #4 (T2, {L1, L3}) have DIFFERENT contexts.\n");
    printf("  - The filter must NOT consider them redundant and will pass both to TSan.\n");
    printf("  - TSan will see two conflicting accesses (#2 and #4) not protected by a common lock and report the race.\n");

    return 0;
}