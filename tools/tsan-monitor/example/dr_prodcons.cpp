// Test: Unsynchronized producer-consumer accessing shared buffer
// Category: Basic Race
// Expectation: RACE
// Notes: Shared buffer and counters without proper synchronization
#include <pthread.h>
#include <unistd.h>

#include <cstdio>

#define BUFFER_SIZE 10

int buffer[BUFFER_SIZE];
int count = 0;  // Current number of elements in buffer

static void* producer(void* arg) {
  int id = *(int*)arg;
  for (int i = 0; i < 5; i++) {
    // Race: count read/write not synchronized
    int index = count;
    buffer[index] =
        id * 100 + i;  // Race: Multiple producers may write to same location
    count++;           // Race: count++ is not atomic
    usleep(10);
  }
  return nullptr;
}

static void* consumer(void*) {
  for (int i = 0; i < 10; i++) {
    // Race: count read/write not synchronized
    if (count > 0) {
      count--;                    // Race: count-- is not atomic
      int value = buffer[count];  // Race: May read uninitialized data
      (void)value;
    }
    usleep(10);
  }
  return nullptr;
}

int main() {
  pthread_t prod1, prod2, cons;
  int id1 = 1, id2 = 2;
  printf("&id1 = %p, &id2 = %p\n", (void*)&id1, (void*)&id2);
  printf("&counter = %p, buffer = %p\n", (void*)&count, (void*)buffer);

  pthread_create(&prod1, nullptr, producer, &id1);
  pthread_create(&prod2, nullptr, producer, &id2);
  pthread_create(&cons, nullptr, consumer, nullptr);

  pthread_join(prod1, nullptr);
  pthread_join(prod2, nullptr);
  pthread_join(cons, nullptr);

  return 0;
}
