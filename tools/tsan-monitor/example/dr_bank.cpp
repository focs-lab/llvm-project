// Test: Race on bank account balance updates
// Category: Basic Race
// Expectation: RACE
// Notes: Concurrent updates to shared balance without synchronization
#include <pthread.h>
#include <unistd.h>

#include <cstdio>

int balance = 1000;

static void* deposit(void* arg) {
  int amount = *(int*)arg;
  // Race: Read, compute, write are not atomic
  int current = balance;
  usleep(100);  // Increase race window
  balance = current + amount;
  return nullptr;
}

static void* withdraw(void* arg) {
  int amount = *(int*)arg;
  // Race: Read, compute, write are not atomic
  int current = balance;
  usleep(100);  // Increase race window
  balance = current - amount;
  return nullptr;
}

int main() {
  pthread_t t1, t2, t3;
  int deposit_amount = 500;
  int withdraw_amount = 200;
  printf("&deposit_amount = %p\n", (void*)&deposit_amount);
  printf("&withdraw_amount = %p\n", (void*)&withdraw_amount);
  printf("&balance = %p\n", (void*)&balance);

  // Three threads operate on balance simultaneously
  pthread_create(&t1, nullptr, deposit, &deposit_amount);
  pthread_create(&t2, nullptr, withdraw, &withdraw_amount);
  pthread_create(&t3, nullptr, deposit, &deposit_amount);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  pthread_join(t3, nullptr);

  // Expected: 1000 + 500 - 200 + 500 = 1800
  // Actual: Possible lost update
  return 0;
}
