// 数组元素竞争：多线程操作数组边界
// 预期检测：Write-Write race on array[4] (边界元素)
#include <pthread.h>
#include <cstdio>

#define ARRAY_SIZE 10

int shared_array[ARRAY_SIZE] = {0};

static void *writer_low(void *) {
  // 写入低位索引
  for (int i = 0; i < 5; i++) {
    shared_array[i] = i + 1;
  }
  return nullptr;
}

static void *writer_high(void *) {
  // 写入高位索引
  for (int i = 4; i < 9; i++) {
    shared_array[i] = (i + 1) * 10;
  }
  return nullptr;
}

static void *reader_all(void *) {
  int sum = 0;
  for (int i = 0; i < ARRAY_SIZE; i++) {
    sum += shared_array[i];  // Race: 读取正在被写入的数组
  }
  (void)sum;
  return nullptr;
}

int main() {
  pthread_t w1, w2, r;
  printf("&arr[4] = %p\n", (void*)&shared_array[4]);

  pthread_create(&w1, nullptr, writer_low, nullptr);
  pthread_create(&w2, nullptr, writer_high, nullptr);  // Race: 索引4同时被两个线程写
  // pthread_create(&r, nullptr, reader_all, nullptr);

  pthread_join(w1, nullptr);
  pthread_join(w2, nullptr);
  // pthread_join(r, nullptr);

  return 0;
}
