// Test: Race on struct member access
// Category: Basic Race
// Expectation: RACE
// Notes: Concurrent writes to struct members without synchronization
#include <pthread.h>
#include <unistd.h>

struct Point {
  int x;
  int y;
};

Point point = {0, 0};

static void* mover_x(void*) {
  for (int i = 0; i < 100; i++) {
    // Race: Read-modify-write of point.x is not atomic
    int current_x = point.x;
    usleep(1);
    point.x = current_x + 1;
  }
  return nullptr;
}

static void* mover_y(void*) {
  for (int i = 0; i < 100; i++) {
    // Race: Read-modify-write of point.y is not atomic
    int current_y = point.y;
    usleep(1);
    point.y = current_y + 1;
  }
  return nullptr;
}

static void* mover_diagonal(void*) {
  for (int i = 0; i < 100; i++) {
    // Race: Modify x and y simultaneously
    int current_x = point.x;
    int current_y = point.y;
    usleep(1);
    point.x = current_x + 1;
    point.y = current_y + 1;
  }
  return nullptr;
}

int main() {
  pthread_t t1, t2, t3;

  pthread_create(&t1, nullptr, mover_x, nullptr);
  pthread_create(&t2, nullptr, mover_y, nullptr);
  pthread_create(&t3, nullptr, mover_diagonal,
                 nullptr);  // Without t3, no race will be detected

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  pthread_join(t3, nullptr);

  return 0;
}
