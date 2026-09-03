; A pointer loaded from a published slot may have been written by another
; thread; the dereference must be instrumented. The slot is escaped, but the
; query OR-ed nothing: each pointee visited overwrote the reason of the one
; before, and the last -- a local never published on its own -- reset it to
; "none". The control loads from a slot that is never published.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@sink = global ptr null, align 8

define void @load_from_published_slot() sanitize_thread {
; CHECK-LABEL: @load_from_published_slot
; CHECK:       call void @__tsan_write4(ptr %l)
entry:
  %s = alloca ptr, align 8
  %q = alloca i32, align 4
  store ptr %s, ptr @sink, align 8
  store ptr %q, ptr %s, align 8
  %l = load ptr, ptr %s, align 8
  store i32 1, ptr %l, align 4
  ret void
}

define void @load_from_private_slot() sanitize_thread {
; CHECK-LABEL: @load_from_private_slot
; CHECK-NOT:   call void @__tsan_write4(ptr %l)
; CHECK:       ret void
entry:
  %s = alloca ptr, align 8
  %q = alloca i32, align 4
  store ptr %q, ptr %s, align 8
  %l = load ptr, ptr %s, align 8
  store i32 1, ptr %l, align 4
  ret void
}
