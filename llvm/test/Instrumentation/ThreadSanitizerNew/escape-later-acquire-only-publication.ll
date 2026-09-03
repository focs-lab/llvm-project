; The sound flow-sensitive rule's third release-like escape: an atomic
; release store of the local's address into a local-linkage global every
; load of which is an acquire. Whoever reads the pointer acquires, so the
; access before the publication happens-before every access through it and
; needs no instrumentation. In @plain_reader one load of the slot is not
; an acquire, so the publication is not release-like and the access stays.
; The publication sits in a later block: the per-block query already treats
; a same-block publication as an escape at the access.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@acq_slot = internal global ptr null, align 8
@mixed_slot = internal global ptr null, align 8

; CHECK-LABEL: @acquire_only
; CHECK-NOT: call void @__tsan_write4
; CHECK: call void @__tsan_atomic64_store
define void @acquire_only() sanitize_thread {
entry:
  %l = alloca i32, align 4
  store i32 1, ptr %l, align 4
  br label %pub
pub:
  store atomic ptr %l, ptr @acq_slot release, align 8
  ret void
}

define ptr @acq_reader() {
  %p = load atomic ptr, ptr @acq_slot acquire, align 8
  ret ptr %p
}

; CHECK-LABEL: @plain_reader
; CHECK: call void @__tsan_write4(ptr %l)
define void @plain_reader() sanitize_thread {
entry:
  %l = alloca i32, align 4
  store i32 1, ptr %l, align 4
  br label %pub
pub:
  store atomic ptr %l, ptr @mixed_slot release, align 8
  ret void
}

define ptr @mixed_reader() {
  %p = load ptr, ptr @mixed_slot, align 8
  ret ptr %p
}
