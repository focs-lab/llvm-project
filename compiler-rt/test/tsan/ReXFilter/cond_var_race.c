#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

long shared_data = 0;
int work_done = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

// Worker thread: waits for signal, then reads data
void* worker_thread(void* arg) {
  pthread_mutex_lock(&mutex);
  printf("Worker: locked, waiting for signal...\n");

  while (work_done == 0) {
    // Magic happens here: unlock -> wait -> lock
    pthread_cond_wait(&cond, &mutex);
  }

  // This access should NOT be filtered because new HB-events
  // occurred after cond_wait.
  printf("Worker: woken up, reading data: %ld\n", shared_data);

  pthread_mutex_unlock(&mutex);
  return NULL;
}

// Racy thread: writes to data WITHOUT locking while worker sleeps
void* racy_thread(void* arg) {
  // Give worker time to sleep
  sleep(1); 
  printf("Racy thread: writing to shared_data without lock!\n");
  // This write conflicts with the read in worker_thread
  shared_data = 42; 
  return NULL;
}

// Main thread: starts everything and signals
int main() {
  pthread_t worker, racy;

  pthread_create(&worker, NULL, worker_thread, NULL);
  pthread_create(&racy, NULL, racy_thread, NULL);

  // Let everything start
  sleep(2); 

  // Signal the worker
  pthread_mutex_lock(&mutex);
  printf("Main: setting work_done and signaling.\n");
  work_done = 1;
  pthread_cond_signal(&cond);
  pthread_mutex_unlock(&mutex);

  pthread_join(worker, NULL);
  pthread_join(racy, NULL);

  printf("\n--- Test: Race with Conditional Variable ---\n");
  printf("Expected result: TSan MUST report a race.\n");
  printf("Filter behavior:\n");
  printf("  - Filter should NOT consider the access in worker_thread after cond_wait redundant.\n");
  printf("  - TSan should see the write from racy_thread and read from worker_thread,\n");
  printf("    which are unordered, and report the race.\n");

  return 0;
}