This document will briefly list the known issues (unsupported scenarios)

C++ standard library: We know that C++ standard library functions have different implementations (for example, g++ and clang++ are completely different, using `libstdc++` and `libc++` respectively), but they all ultimately call standard library functions provided by `libc` (such as `pthread_mutex_lock`, `pthread_create`). However, I found that before C++ standard library functions call `libc` functions, there are some additional memory accesses, which can cause the monitor to incorrectly report races—something that does not happen with native tsan (which may use Ignore-related techniques I guess). I made some simple attempts to address this, but the results were not ideal. You can refer to the `user/zengyan/wip/ignore-events` branch ([GitHub - focs-lab/llvm-project at user/zengyan/wip/ignore-events](https://github.com/focs-lab/llvm-project/tree/user/zengyan/wip/ignore-events)) for details

```cpp
// tools/tsan-monitor/example/mx_rwlock.cpp
// Test: Read-write lock synchronization  
// Category: Mutex  
// Expectation: NO_RACE  
// Notes: Multiple readers and exclusive writer with proper locking  
  
#include <pthread.h>  
#include <stdio.h>  
  
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;  
int shared_value = 0;  
  
void* reader(void* arg) {  
  int id = *(int*)arg;  
  pthread_mutex_lock(&mtx);  
  printf("reader%d: shared_value=%d\n", id, shared_value);  
  pthread_mutex_unlock(&mtx);  
  return NULL;  
}  
  
void* writer(void* arg) {  
  int new_value = *(int*)arg;  
  pthread_mutex_lock(&mtx);  
  shared_value = new_value;  
  pthread_mutex_unlock(&mtx);  
  return NULL;  
}  
  
int main() {  
  pthread_t threads[5];  
  int args[5];  
  
  printf("&mtx = %p\n", (void*)&mtx);  
  printf("&shared_value = %p\n", (void*)&shared_value);  
  printf("&threads = %p\n", (void*)&threads);  
  printf("&writer = %p\n", (void*)writer);  
  printf("&reader = %p\n", (void*)reader);  
  
  args[0] = 10;  
  pthread_create(&threads[0], NULL, writer, &args[0]);  
  
  args[1] = 1;  
  pthread_create(&threads[1], NULL, reader, &args[1]);  
  
  args[2] = 2;  
  pthread_create(&threads[2], NULL, reader, &args[2]);  
  
  args[3] = 11;  
  pthread_create(&threads[3], NULL, writer, &args[3]);  
  
  args[4] = 3;  
  pthread_create(&threads[4], NULL, reader, &args[4]);  
  
  for (int i = 0; i < 5; ++i) {  
    pthread_join(threads[i], NULL);  
  }  
  pthread_mutex_destroy(&mtx);  
  return 0;  
}

// In tsan monitor, if you use libc functions like above, it will not report a race.
// But if you use libc++/libstdc++ functions like `std::thread`, `std::lock_guard`, etc,
// there will be a race reported from monitor
```

Events from different laps may overwrite slots in the channel, resulting in data loss or hanging of monitor: In the monitor, the channel is a fixed-size `RingBuf` created using `mmap`. Threads in the origin program write to this channel, and the `Reader` in the monitor reads from it. Once the channel is full, the next lap starts writing from the beginning. If the `Reader` has not read the event that is about to be overwritten, data loss will occur. This may cause the monitor to produce incorrect race detection results or even hang due to abnormal operation

```cpp
// tools/tsan-monitor/example/at_rel_acq.cpp
// Test: Acquire-release synchronization with atomics  
// Category: Atomics  
// Expectation: NO_RACE  
// Notes: Release by producer and acquire by consumer establish HB  
#include <atomic>  
#include <iostream>  
#include <thread>  
  
namespace {  
std::atomic<int> ready{0};  
int shared_value = 0;  
  
void producer() {  
  shared_value = 42;  
  ready.store(1, std::memory_order_release);  
}  
  
void consumer() {  
  while (ready.load(std::memory_order_acquire) != 1) {  
	  ;
  }  
}  
}  // namespace  
  
int main() {  
  std::thread t1(producer);  // t2(consumer)
  std::thread t2(consumer);  // t1(producer)
  printf("&ready = %p\n", (void*)&ready);  
  printf("&shared_value = %p\n", (void*)&shared_value);  
  printf("&t1 = %p\n", (void*)&t1);  
  printf("&t2 = %p\n", (void*)&t2);  
  t1.join();  
  t2.join();  
  return 0;  
}

// In tsan monitor, it will not report any race.
// But if you change the order of thread creation like comments above,
// monitor cannot work anymore, it will hang forever.
//
// NOTE: you can use `example/run.py` to debug and see what
// happens in monitor, and find the image of each channels.
```