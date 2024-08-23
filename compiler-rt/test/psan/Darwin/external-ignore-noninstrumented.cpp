// RUN: %clangxx_psan -shared %p/external-lib.cpp -fno-sanitize=predict -DUSE_PSAN_CALLBACKS \
// RUN:   -o %t-lib.dylib -install_name @rpath/`basename %t-lib.dylib`

// RUN: %clangxx_psan -shared %p/external-noninstrumented-module.cpp %t-lib.dylib -fno-sanitize=predict \
// RUN:   -o %t-module.dylib -install_name @rpath/`basename %t-module.dylib`

// RUN: %clangxx_psan %s %t-module.dylib -o %t
// RUN: %run %t 2>&1 | FileCheck %s

#include <stdio.h>

extern "C" void NonInstrumentedModule();
int main(int argc, char *argv[]) {
  NonInstrumentedModule();
  fprintf(stderr, "Done.\n");
}

// CHECK-NOT: WARNING: PredictiveSanitizer
// CHECK: Done.
