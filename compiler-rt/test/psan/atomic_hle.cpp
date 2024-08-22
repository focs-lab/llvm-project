// RUN: %clangxx_psan -O1 %s -o %t && %run %t 2>&1 | FileCheck %s
#include "test.h"
#include <sanitizer/psan_interface_atomic.h>

#ifndef __ATOMIC_HLE_ACQUIRE
#define __ATOMIC_HLE_ACQUIRE (1 << 16)
#endif
#ifndef __ATOMIC_HLE_RELEASE
#define __ATOMIC_HLE_RELEASE (1 << 17)
#endif

int main() {
  volatile int x = 0;
  //__atomic_fetch_add(&x, 1, __ATOMIC_ACQUIRE | __ATOMIC_HLE_ACQUIRE);
  //__atomic_store_n(&x, 0, __ATOMIC_RELEASE | __ATOMIC_HLE_RELEASE);
  __psan_atomic32_fetch_add(&x, 1,
      (__psan_memory_order)(__ATOMIC_ACQUIRE | __ATOMIC_HLE_ACQUIRE));
  __psan_atomic32_store(&x, 0,
      (__psan_memory_order)(__ATOMIC_RELEASE | __ATOMIC_HLE_RELEASE));
  fprintf(stderr, "DONE\n");
  return 0;
}

// CHECK: DONE

