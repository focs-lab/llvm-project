// Test: Race during linked list operations
// Category: Basic Race
// Expectation: RACE
// Notes: Concurrent insertions touching shared head
#include <pthread.h>
#include <stdlib.h>

struct Node {
  int data;
  Node* next;
};

Node* head = nullptr;

// Incorrect linked list insertion (no synchronization)
void insert(int value) {
  Node* new_node = (Node*)malloc(sizeof(Node));
  new_node->data = value;

  // Race: Multiple threads modify head simultaneously
  new_node->next = head;  // Race: read head
  head = new_node;        // Race: write head
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
    // Race: List may be modified while traversing
    Node* current = head;
    int count = 0;
    while (current != nullptr) {
      count++;
      current = current->next;  // May access nodes being inserted
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

  // Note: Real applications need to clean up memory, simplified here
  return 0;
}
