#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define NUM_THREADS 2
#define LOOP_COUNT 2

// The "haystack": data that is always correctly protected.
// Many filtered events should be generated here.
long safe_but_redundant_counter = 0;
pthread_mutex_t safe_mutex;

// The "needle": data that is subject to a data race.
long racy_counter = 0;
pthread_barrier_t barrier;

void* thread_func(void* arg) {
    long thread_id = (long)arg;

    for (int i = 0; i < LOOP_COUNT; ++i) {
        // --- Part 1: Generate "noise" from redundant events ---
/*
        pthread_mutex_lock(&safe_mutex);
        safe_but_redundant_counter++;
        pthread_mutex_unlock(&safe_mutex);
*/

        // --- Part 2: Hide the race ---
        // Only one thread writes, the others read without a lock.
        if (thread_id == 0) {
            // This write will conflict with the reads from other threads.
            printf("T%ld: Writing to racy_counter\n", thread_id);
            racy_counter = 111;
        } else {
            // These reads will conflict with the write from thread 0.
            if (racy_counter > 1) { // Some unpredictable read
                 // (just to have the access)
            }
        }
    }
    pthread_barrier_wait(&barrier);
    return NULL;
}

__attribute__((no_sanitize("thread")))
int main() {
    printf("Address of racy_counter: %p\n", (void*)&racy_counter);

    pthread_t threads[NUM_THREADS];
    pthread_barrier_init(&barrier, NULL, NUM_THREADS);
    pthread_mutex_init(&safe_mutex, NULL);

    for (long i = 0; i < NUM_THREADS; ++i) {
        pthread_create(&threads[i], NULL, thread_func, (void*)i);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&safe_mutex);
    pthread_barrier_destroy(&barrier);

    printf("--- Test: 'Needle in a Haystack' Race ---\n");
    printf("Expected result: TSan MUST report a race on 'racy_counter'.\n");
    printf("Filter behavior:\n");
    printf("  - For 'safe_but_redundant_counter': the filter should prune ~%d of %d accesses.\n",
           (NUM_THREADS * LOOP_COUNT) - 2, NUM_THREADS * LOOP_COUNT);
    printf("  - For 'racy_counter': all accesses have an empty context {}. The filter will pass\n");
    printf("    the first write from T0 and the first read from T1. This is SUFFICIENT for TSan to find the race.\n");
    printf("  - If the filter mistakenly associates the context from 'safe_mutex' with the access to 'racy_counter',\n");
    printf("    it might miss the race. This test checks that this does not happen.\n");

    return 0;
}