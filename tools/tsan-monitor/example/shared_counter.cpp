// 共享计数器竞争：多个计数器的非原子递增
// 预期检测：多个 Write-Write race on counter1, counter2, counter3
#include <pthread.h>

int counter1 = 0;
int counter2 = 0;
int counter3 = 0;

static void *increment_all(void *) {
  for (int i = 0; i < 1000; i++) {
    // Race: 三个计数器的读-改-写都不是原子的
    counter1 = counter1 + 1;
    counter2 = counter2 + 1;
    counter3 = counter3 + 1;
  }
  return nullptr;
}

static void *decrement_all(void *) {
  for (int i = 0; i < 1000; i++) {
    // Race: 三个计数器的读-改-写都不是原子的
    counter1 = counter1 - 1;
    counter2 = counter2 - 1;
    counter3 = counter3 - 1;
  }
  return nullptr;
}

int main() {
  pthread_t t1, t2, t3, t4;

  // 4个线程同时修改计数器
  pthread_create(&t1, nullptr, increment_all, nullptr);
  pthread_create(&t2, nullptr, increment_all, nullptr);
  pthread_create(&t3, nullptr, decrement_all, nullptr);
  pthread_create(&t4, nullptr, decrement_all, nullptr);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  pthread_join(t3, nullptr);
  pthread_join(t4, nullptr);

  // 预期: 所有计数器都应该是 0
  // 实际: 由于race，结果不可预测
  return 0;
}
