// 链表竞争：并发插入节点
// 预期检测：Write-Read 和 Write-Write race on head 指针
#include <pthread.h>
#include <stdlib.h>

struct Node {
  int data;
  Node* next;
};

Node* head = nullptr;

// 错误的链表插入（没有同步）
void insert(int value) {
  Node* new_node = (Node*)malloc(sizeof(Node));
  new_node->data = value;

  // Race: 多个线程同时修改 head
  new_node->next = head;  // Race: 读取 head
  head = new_node;        // Race: 写入 head
}

static void* inserter(void* arg) {
  int id = *(int*)arg;
  for (int i = 0; i < 10; i++) {
    insert(id * 100 + i);
  }
  return nullptr;
}

static void* reader(void*) {
  for (int i = 0; i < 5; i++) {
    // Race: 遍历链表时，链表可能正在被修改
    Node* current = head;
    int count = 0;
    while (current != nullptr) {
      count++;
      current = current->next;  // 可能访问到正在被插入的节点
    }
    (void)count;
  }
  return nullptr;
}

int main() {
  pthread_t t1, t2, t3, r;
  int id1 = 1, id2 = 2, id3 = 3;

  pthread_create(&t1, nullptr, inserter, &id1);
  pthread_create(&t2, nullptr, inserter, &id2);
  pthread_create(&t3, nullptr, inserter, &id3);
  pthread_create(&r, nullptr, reader, nullptr);

  pthread_join(t1, nullptr);
  pthread_join(t2, nullptr);
  pthread_join(t3, nullptr);
  pthread_join(r, nullptr);

  // 注：实际应用中还需要清理内存，这里简化了
  return 0;
}
