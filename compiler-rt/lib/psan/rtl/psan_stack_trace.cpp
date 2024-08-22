//===-- psan_stack_trace.cpp ----------------------------------------------===//
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
#include "psan_stack_trace.h"
#include "psan_rtl.h"
#include "psan_mman.h"

namespace __psan {

VarSizeStackTrace::VarSizeStackTrace()
    : StackTrace(nullptr, 0), trace_buffer(nullptr) {}

VarSizeStackTrace::~VarSizeStackTrace() {
  ResizeBuffer(0);
}

void VarSizeStackTrace::ResizeBuffer(uptr new_size) {
  Free(trace_buffer);
  trace_buffer = (new_size > 0)
                     ? (uptr *)Alloc(new_size * sizeof(trace_buffer[0]))
                     : nullptr;
  trace = trace_buffer;
  size = new_size;
}

void VarSizeStackTrace::Init(const uptr *pcs, uptr cnt, uptr extra_top_pc) {
  ResizeBuffer(cnt + !!extra_top_pc);
  internal_memcpy(trace_buffer, pcs, cnt * sizeof(trace_buffer[0]));
  if (extra_top_pc)
    trace_buffer[cnt] = extra_top_pc;
}

void VarSizeStackTrace::ReverseOrder() {
  for (u32 i = 0; i < (size >> 1); i++)
    Swap(trace_buffer[i], trace_buffer[size - 1 - i]);
}

}  // namespace __psan

#if !SANITIZER_GO
void __sanitizer::BufferedStackTrace::UnwindImpl(
    uptr pc, uptr bp, void *context, bool request_fast, u32 max_depth) {
  uptr top = 0;
  uptr bottom = 0;
  GetThreadStackTopAndBottom(false, &top, &bottom);
  bool fast = StackTrace::WillUseFastUnwind(request_fast);
  Unwind(max_depth, pc, bp, context, top, bottom, fast);
}
#endif  // SANITIZER_GO
