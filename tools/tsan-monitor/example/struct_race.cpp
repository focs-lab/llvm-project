// 结构体字段竞争：不同字段的并发访问
// 预期检测：Write-Write race on point.x 和 point.y
#include <pthread.h>
#include <unistd.h>

struct Point {
  int x;
  int y;
};

Point point = {0, 0};

static void *mover_x(void *) {
  for (int i = 0; i < 100; i++) {
    // Race: 读-改-写 point.x 不是原子的
    int current_x = point.x;
    usleep(1);
    point.x = current_x + 1;
  }
  return nullptr;
}

static void *mover_y(void *) {
  for (int i = 0; i < 100; i++) {
    // Race: 读-改-写 point.y 不是原子的
    int current_y = point.y;
    usleep(1);
    point.y = current_y + 1;
  }
  return nullptr;
}

static void *mover_diagonal(void *) {
  for (int i = 0; i < 100; i++) {
    // Race: 同时修改 x 和 y
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
  pthread_create(&t3, nullptr, mover_diagonal, nullptr); // 若没有 t3 则不会检测到 race

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  pthread_join(t3, nullptr);

  return 0;
}
