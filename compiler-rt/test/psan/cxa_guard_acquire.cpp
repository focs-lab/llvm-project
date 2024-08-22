// RUN: %clangxx_psan -O1 %s -o %t && %run %t 2>&1 | FileCheck %s

#include <sanitizer/psan_interface.h>
#include <stdio.h>

namespace __psan {

#if (__APPLE__)
__attribute__((weak))
#endif
void OnPotentiallyBlockingRegionBegin() {
  printf("Enter __cxa_guard_acquire\n");
}

#if (__APPLE__)
__attribute__((weak))
#endif
void OnPotentiallyBlockingRegionEnd() { printf("Exit __cxa_guard_acquire\n"); }

} // namespace __psan

int main(int argc, char **argv) {
  // CHECK: Enter main
  printf("Enter main\n");
  // CHECK-NEXT: Enter __cxa_guard_acquire
  // CHECK-NEXT: Exit __cxa_guard_acquire
  static int s = argc;
  (void)s;
  // CHECK-NEXT: Exit main
  printf("Exit main\n");
  return 0;
}
