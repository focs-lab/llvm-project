// RUN: %clang_psan -O2 %s -o %t
// RUN: %env_psan_opts=check_printf=1 %run %t 2>&1 | FileCheck %s
// RUN: %env_psan_opts=check_printf=0 %run %t 2>&1 | FileCheck %s
// RUN: %run %t 2>&1 | FileCheck %s

#include <stdio.h>
int main() {
  volatile char c = '0';
  volatile int x = 12;
  volatile float f = 1.239;
  volatile char s[] = "34";
  printf("%c %d %.3f %s\n", c, x, f, s);
  return 0;
  // Check that printf works fine under Psan.
  // CHECK: 0 12 1.239 34
}
