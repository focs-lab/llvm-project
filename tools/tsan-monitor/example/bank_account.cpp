// 银行账户竞争：多线程并发修改余额
// 预期检测：Write-Write race on balance
#include <pthread.h>
#include <unistd.h>

#include <cstdio>

int balance = 1000;

static void* deposit(void* arg) {
  int amount = *(int*)arg;
  // Race: 读取、计算、写入不是原子的
  int current = balance;
  usleep(100);  // 增加race窗口
  balance = current + amount;
  return nullptr;
}

static void* withdraw(void* arg) {
  int amount = *(int*)arg;
  // Race: 读取、计算、写入不是原子的
  int current = balance;
  usleep(100);  // 增加race窗口
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

  // 三个线程同时操作balance
  pthread_create(&t1, nullptr, deposit, &deposit_amount);
  pthread_create(&t2, nullptr, withdraw, &withdraw_amount);
  pthread_create(&t3, nullptr, deposit, &deposit_amount);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  pthread_join(t3, nullptr);

  // 预期: 1000 + 500 - 200 + 500 = 1800
  // 实际: 可能丢失更新（lost update）
  return 0;
}
