// Test that verifies PSan runtime doesn't contain compiler-emitted
// memcpy/memmove calls. It builds the binary with PSan and check's
// its objdump.

// This could fail if using a static libunwind because that static libunwind
// could be uninstrumented and contain memcpy/memmove calls not intercepted by
// psan.
// REQUIRES: shared_unwind, x86_64-target-arch

// RUN: %clang_psan -O1 %s -o %t
// RUN: llvm-objdump -d -l %t | FileCheck --implicit-check-not="{{(callq|jmpq) .*<(__interceptor_.*)?mem(cpy|set|move)>}}" %s

int main() { return 0; }
