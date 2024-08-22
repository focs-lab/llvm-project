//===-- psan_interface.cpp ------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PredictiveSanitizer (PSan), a predictive race detector.
// This is a fork of ThreadSanitizer (TSan) at LLVM commit
// c609043dd00955bf177ff57b0bad2a87c1e61a36.
//
//===----------------------------------------------------------------------===//

#include "psan_interface.h"
#include "psan_interface_ann.h"
#include "psan_rtl.h"
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_ptrauth.h"

#define CALLERPC ((uptr)__builtin_return_address(0))

using namespace __psan;

void __psan_init() { Initialize(cur_thread_init()); }

void __psan_flush_memory() {
  FlushShadowMemory();
}

void __psan_read16_pc(void *addr, void *pc) {
  uptr pc_no_pac = STRIP_PAC_PC(pc);
  ThreadState *thr = cur_thread();
  MemoryAccess(thr, pc_no_pac, (uptr)addr, 8, kAccessRead);
  MemoryAccess(thr, pc_no_pac, (uptr)addr + 8, 8, kAccessRead);
}

void __psan_write16_pc(void *addr, void *pc) {
  uptr pc_no_pac = STRIP_PAC_PC(pc);
  ThreadState *thr = cur_thread();
  MemoryAccess(thr, pc_no_pac, (uptr)addr, 8, kAccessWrite);
  MemoryAccess(thr, pc_no_pac, (uptr)addr + 8, 8, kAccessWrite);
}

// __psan_unaligned_read/write calls are emitted by compiler.

void __psan_unaligned_read16(const void *addr) {
  uptr pc = CALLERPC;
  ThreadState *thr = cur_thread();
  UnalignedMemoryAccess(thr, pc, (uptr)addr, 8, kAccessRead);
  UnalignedMemoryAccess(thr, pc, (uptr)addr + 8, 8, kAccessRead);
}

void __psan_unaligned_write16(void *addr) {
  uptr pc = CALLERPC;
  ThreadState *thr = cur_thread();
  UnalignedMemoryAccess(thr, pc, (uptr)addr, 8, kAccessWrite);
  UnalignedMemoryAccess(thr, pc, (uptr)addr + 8, 8, kAccessWrite);
}

extern "C" {
SANITIZER_INTERFACE_ATTRIBUTE
void *__psan_get_current_fiber() {
  return cur_thread();
}

SANITIZER_INTERFACE_ATTRIBUTE
void *__psan_create_fiber(unsigned flags) {
  return FiberCreate(cur_thread(), CALLERPC, flags);
}

SANITIZER_INTERFACE_ATTRIBUTE
void __psan_destroy_fiber(void *fiber) {
  FiberDestroy(cur_thread(), CALLERPC, static_cast<ThreadState *>(fiber));
}

SANITIZER_INTERFACE_ATTRIBUTE
void __psan_switch_to_fiber(void *fiber, unsigned flags) {
  FiberSwitch(cur_thread(), CALLERPC, static_cast<ThreadState *>(fiber), flags);
}

SANITIZER_INTERFACE_ATTRIBUTE
void __psan_set_fiber_name(void *fiber, const char *name) {
  ThreadSetName(static_cast<ThreadState *>(fiber), name);
}
}  // extern "C"

void __psan_acquire(void *addr) {
  Acquire(cur_thread(), CALLERPC, (uptr)addr);
}

void __psan_release(void *addr) {
  Release(cur_thread(), CALLERPC, (uptr)addr);
}
