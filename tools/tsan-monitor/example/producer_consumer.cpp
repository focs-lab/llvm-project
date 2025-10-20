// 生产者-消费者竞争：缺少同步的共享缓冲区
// 预期检测：多个 Write-Read race on buffer[] 和 count
#include <pthread.h>
#include <unistd.h>

#include <cstdio>

#define BUFFER_SIZE 10

int buffer[BUFFER_SIZE];
int count = 0;  // 当前buffer中的元素个数

static void* producer(void* arg) {
  int id = *(int*)arg;
  for (int i = 0; i < 5; i++) {
    // Race: count 的读写没有同步
    int index = count;
    buffer[index] = id * 100 + i;  // Race: 可能多个producer写同一位置
    count++;                       // Race: count++ 不是原子的
    usleep(10);
  }
  return nullptr;
}

static void* consumer(void*) {
  for (int i = 0; i < 10; i++) {
    // Race: count 的读写没有同步
    if (count > 0) {
      count--;                    // Race: count-- 不是原子的
      int value = buffer[count];  // Race: 可能读到未初始化的数据
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
