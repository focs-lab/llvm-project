// Test: Multiple readers, single writer pattern with mutex
// Expected: NO RACE (all accesses protected by mutex)

#include <cstdio>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::mutex mtx;
int shared_value = 0;

void reader(int id) {
  std::lock_guard<std::mutex> lock(mtx);
  printf("reader%d: shared_value=%d\n", id, shared_value);
}

void writer(int new_value) {
  std::lock_guard<std::mutex> lock(mtx);
  shared_value = new_value;
}
}  // namespace

int main() {
  std::vector<std::thread> threads;
  printf("&mtx = %p\n", (void*)&mtx);
  printf("&shared_value = %p\n", (void*)&shared_value);
  printf("&threads = %p\n", (void*)&threads);
  printf("&writer = %p\n", (void*)writer);
  printf("&reader = %p\n", (void*)reader);
  threads.emplace_back(writer, 10);  // thread-1
  threads.emplace_back(reader, 1);   // thread-2
  threads.emplace_back(reader, 2);   // thread-3
  threads.emplace_back(writer, 11);  // thread-4
  threads.emplace_back(reader, 3);   // thread-5

  for (auto& t : threads) {
    t.join();
  }
  return 0;
}
