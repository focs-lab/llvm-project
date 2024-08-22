// Test that we can include header with PSan atomic interface.
// RUN: %clang_psan %s -o %t && %run %t 2>&1 | FileCheck %s
#include <sanitizer/psan_interface_atomic.h>
#include <stdio.h>

int main() {
  __psan_atomic32 a;
  __psan_atomic32_store(&a, 100, __psan_memory_order_release);
  int res = __psan_atomic32_load(&a, __psan_memory_order_acquire);
  if (res == 100) {
    // CHECK: PASS
    fprintf(stderr, "PASS\n");
    return 0;
  }
  return 1;
}
