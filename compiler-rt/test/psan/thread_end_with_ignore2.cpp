// RUN: %clangxx_psan -O1 %s -o %t && %deflake %run %t | FileCheck %s

// ReportIgnoresEnabled is disabled on Darwin, see comment in psan_rtl_thread.cpp.
// UNSUPPORTED: darwin
#include "test.h"

int main() {
  AnnotateIgnoreWritesBegin("", 0);
}

// CHECK: PredictiveSanitizer: main thread finished with ignores enabled
// CHECK:   Ignore was enabled at:
// CHECK:     #0 AnnotateIgnoreWritesBegin
// CHECK:     #1 main

