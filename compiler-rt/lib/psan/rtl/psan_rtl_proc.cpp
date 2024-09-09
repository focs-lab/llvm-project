//===-- psan_rtl_proc.cpp -----------------------------------------------===//
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

#include "sanitizer_common/sanitizer_placement_new.h"
#include "psan_rtl.h"
#include "psan_mman.h"
#include "psan_flags.h"

namespace __psan {

Processor *ProcCreate() {
  void *mem = InternalAlloc(sizeof(Processor));
  internal_memset(mem, 0, sizeof(Processor));
  Processor *proc = new(mem) Processor;
  proc->thr = nullptr;
#if !SANITIZER_GO
  AllocatorProcStart(proc);
#endif
  if (common_flags()->detect_deadlocks)
    proc->dd_pt = ctx->dd->CreatePhysicalThread();

{
  Lock l(&ctx->shadow_alloc_mtx);
  if (ctx->shadow_alloc_queue.Empty()) {
    void *sa_mem = InternalAlloc(sizeof(HBShadowCellAlloc));
    internal_memset(sa_mem, 0, sizeof(HBShadowCellAlloc));
    HBShadowCellAlloc *shadow_alloc = new(sa_mem) HBShadowCellAlloc;
    proc->shadow_alloc = shadow_alloc;
  }
  else {
    num_shadow_alloc_recycles++;
    // Printf("ShadowAlloc Recycle %u\n", num_shadow_alloc_recycles);
    proc->shadow_alloc = ctx->shadow_alloc_queue.PopFront();
  }
}
  return proc;
}

void ProcDestroy(Processor *proc) {
  CHECK_EQ(proc->thr, nullptr);
#if !SANITIZER_GO
  AllocatorProcFinish(proc);
#endif
  ctx->metamap.OnProcIdle(proc);
  if (common_flags()->detect_deadlocks)
     ctx->dd->DestroyPhysicalThread(proc->dd_pt);

  {
    Lock l(&ctx->shadow_alloc_mtx);
    ctx->shadow_alloc_queue.PushFront(proc->shadow_alloc);
    proc->shadow_alloc = nullptr;
  }

  proc->~Processor();
  InternalFree(proc);
}

void ProcWire(Processor *proc, ThreadState *thr) {
  CHECK_EQ(thr->proc1, nullptr);
  CHECK_EQ(proc->thr, nullptr);
  thr->proc1 = proc;
  proc->thr = thr;
}

void ProcUnwire(Processor *proc, ThreadState *thr) {
  CHECK_EQ(thr->proc1, proc);
  CHECK_EQ(proc->thr, thr);
  thr->proc1 = nullptr;
  proc->thr = nullptr;
}

}  // namespace __psan
