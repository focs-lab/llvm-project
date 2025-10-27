// Test: Race during lazy initialization
// Category: Basic Race
// Expectation: RACE
// Notes: Double-checked locking anti-pattern without proper synchronization
#include <pthread.h>

class Singleton {
 public:
  int data;
  Singleton() : data(42) {}
};

Singleton* singleton = nullptr;

// Incorrect lazy initialization implementation (missing synchronization)
Singleton* GetInstance() {
  if (singleton ==
      nullptr) {  // Race: Multiple threads may see nullptr simultaneously
    singleton = new Singleton();  // Race: Multiple threads may create objects
                                  // simultaneously
  }
  return singleton;
}

static void* user_thread(void* arg) {
  Singleton* instance = GetInstance();
  int value = instance->data;  // May read incomplete object
  (void)value;
  return nullptr;
}

int main() {
  pthread_t threads[5];

  // 5 threads call GetInstance simultaneously
  for (int i = 0; i < 5; i++) {
    pthread_create(&threads[i], nullptr, user_thread, nullptr);
  }

  for (int i = 0; i < 5; i++) {
    pthread_join(threads[i], nullptr);
  }

  return 0;
}
