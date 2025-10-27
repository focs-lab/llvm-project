// Test: Races on array elements with overlapping writes
// Category: Basic Race
// Expectation: RACE
// Notes: Overlap on index 4 between two writers
#include <pthread.h>

#include <cstdio>

#define ARRAY_SIZE 10

int shared_array[ARRAY_SIZE] = {0};

static void* writer_low(void*) {
  // Write to low indices
  for (int i = 0; i < 5; i++) {
    shared_array[i] = i + 1;
  }
  return nullptr;
}

static void* writer_high(void*) {
  // Write to high indices
  for (int i = 4; i < 9; i++) {
    shared_array[i] = (i + 1) * 10;
  }
  return nullptr;
}

static void* reader_all(void*) {
  int sum = 0;
  for (int i = 0; i < ARRAY_SIZE; i++) {
    sum += shared_array[i];  // Race: Reading array while being written to
  }
  (void)sum;
  return nullptr;
}

int main() {
  pthread_t w1, w2, r;
  printf("&arr[4] = %p\n", (void*)&shared_array[4]);

  pthread_create(&w1, nullptr, writer_low, nullptr);
  pthread_create(
      &w2, nullptr, writer_high,
      nullptr);  // Race: Index 4 written by two threads simultaneously
  // pthread_create(&r, nullptr, reader_all, nullptr);

  pthread_join(w1, nullptr);
  pthread_join(w2, nullptr);
  // pthread_join(r, nullptr);

  return 0;
}
