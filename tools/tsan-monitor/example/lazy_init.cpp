// 懒加载初始化竞争：双重检查锁定反模式（无锁保护）
// 预期检测：Write-Read race on singleton
#include <pthread.h>

class Singleton {
 public:
  int data;
  Singleton() : data(42) {}
};

Singleton* singleton = nullptr;

// 错误的懒加载实现（缺少同步）
Singleton* GetInstance() {
  if (singleton == nullptr) {     // Race: 多线程可能同时看到 nullptr
    singleton = new Singleton();  // Race: 多线程可能同时创建对象
  }
  return singleton;
}

static void* user_thread(void* arg) {
  Singleton* instance = GetInstance();
  int value = instance->data;  // 可能读到不完整的对象
  (void)value;
  return nullptr;
}

int main() {
  pthread_t threads[5];

  // 5个线程同时调用GetInstance
  for (int i = 0; i < 5; i++) {
    pthread_create(&threads[i], nullptr, user_thread, nullptr);
  }

  for (int i = 0; i < 5; i++) {
    pthread_join(threads[i], nullptr);
  }

  return 0;
}
