// Check if psan work with PIE binaries.
// RUN: %clang_psan %s -pie -fpic -o %t && %run %t

int main(void) {
  return 0;
}
