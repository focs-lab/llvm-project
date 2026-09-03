; A pointer stored into memory that is published -- before or after the store
; -- has escaped with it. The "after" order worked (the publication walks the
; container's pointees); the "before" order did not: storing into an already
; published container consulted only whether the container was a global, an
; argument or a call result, never whether the analysis had already found it
; escaped. The control stores into a container that is never published.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@sink = global ptr null, align 8

define void @container_published_first() sanitize_thread {
; CHECK-LABEL: @container_published_first
; CHECK:       call void @__tsan_write4(ptr %x)
entry:
  %s = alloca ptr, align 8
  %x = alloca i32, align 4
  store ptr %s, ptr @sink, align 8
  store ptr %x, ptr %s, align 8
  store i32 1, ptr %x, align 4
  ret void
}

define void @container_published_after() sanitize_thread {
; CHECK-LABEL: @container_published_after
; CHECK:       call void @__tsan_write4(ptr %x)
entry:
  %s = alloca ptr, align 8
  %x = alloca i32, align 4
  store ptr %x, ptr %s, align 8
  store ptr %s, ptr @sink, align 8
  store i32 1, ptr %x, align 4
  ret void
}

define void @container_never_published() sanitize_thread {
; CHECK-LABEL: @container_never_published
; CHECK-NOT:   call void @__tsan_write4(ptr %x)
; CHECK:       ret void
entry:
  %s = alloca ptr, align 8
  %x = alloca i32, align 4
  store ptr %x, ptr %s, align 8
  store i32 1, ptr %x, align 4
  ret void
}
