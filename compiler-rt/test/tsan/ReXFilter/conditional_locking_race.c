#include <pthread.h>
#include <stdio.h>

#define NUM_THREADS 2

long shared_data = 0;
pthread_mutex_t mutex1;

// Threads with even IDs acquire the lock, odd ones do not.
void* thread_func(void* arg) {
    long thread_id = (long)arg;
    if (thread_id % 2 == 0)
        pthread_mutex_lock(&mutex1);

    // This memory access is performed by all threads.
    // For even threads, the context is {L1}.
    // For odd threads, the context is {}.
    // This creates a race between even and odd threads.
    shared_data++;

    if (thread_id % 2 == 0)
        pthread_mutex_unlock(&mutex1);

    return NULL;
}

__attribute__((no_sanitize("thread")))
int main() {
    pthread_t threads[NUM_THREADS];
    pthread_mutex_init(&mutex1, NULL);

    for (long i = 0; i < NUM_THREADS; ++i)
        pthread_create(&threads[i], NULL, thread_func, (void*)i);

    for (int i = 0; i < NUM_THREADS; ++i)
        pthread_join(threads[i], NULL);

    pthread_mutex_destroy(&mutex1);

    printf("--- Test: Conditional Locking and Race ---\n");
    printf("Expected result: TSan MUST report a data race.\n");
    printf("Filter behavior:\n");
    printf("  - Thread 0 accesses the data with context {L1}.\n");
    printf("  - Thread 1 accesses the data with an empty context {}.\n");
    printf("  - Since the contexts are different, the filter will pass both events to TSan.\n");
    printf("  - TSan will see the conflict and report the race.\n");
    return 0;
}