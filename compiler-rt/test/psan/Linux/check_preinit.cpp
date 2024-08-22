// RUN: %clang_psan -fno-sanitize=thread -shared -fPIC -O1 -DBUILD_SO=1 %s -o \
// RUN:  %t.so && \
// RUN:   %clang_psan -O1 %s %t.so -o %t && %run %t 2>&1 | FileCheck %s
// RUN: llvm-objdump -t %t | FileCheck %s --check-prefix=CHECK-DUMP
// CHECK-DUMP:  {{[.]preinit_array.*__local_psan_preinit}}

// SANITIZER_CAN_USE_PREINIT_ARRAY is undefined on android.
// UNSUPPORTED: android

// Test checks if __psan_init is called from .preinit_array.
// Without initialization from .preinit_array, __psan_init will be called from
// constructors of the binary which are called after constructors of shared
// library.

#include <sanitizer/psan_interface.h>
#include <stdio.h>

#if BUILD_SO

// "volatile" is needed to avoid compiler optimize-out constructors.
volatile int counter = 0;
volatile int lib_constructor_call = 0;
volatile int psan_init_call = 0;

__attribute__ ((constructor))
void LibConstructor() {
  lib_constructor_call = ++counter;
};

#else  // BUILD_SO

extern int counter;
extern int lib_constructor_call;
extern int psan_init_call;

volatile int bin_constructor_call = 0;

__attribute__ ((constructor))
void BinConstructor() {
  bin_constructor_call = ++counter;
};

namespace __psan {

void OnInitialize() {
  psan_init_call = ++counter;
}

}

int main() {
  // CHECK: PSAN_INIT 1
  // CHECK: LIB_CONSTRUCTOR 2
  // CHECK: BIN_CONSTRUCTOR 3
  printf("PSAN_INIT %d\n", psan_init_call);
  printf("LIB_CONSTRUCTOR %d\n", lib_constructor_call);
  printf("BIN_CONSTRUCTOR %d\n", bin_constructor_call);
  return 0;
}

#endif  // BUILD_SO
