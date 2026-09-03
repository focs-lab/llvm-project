; A pointer loaded from a slot the analysis never saw a store into may point
; anywhere -- here into the caller's memory, copied in by memcpy. The slot had
; no recorded pointee, the pointee loop ran zero times, and the dereference was
; treated as local. The control's slot holds a pointer to a private local.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define void @pointee_from_memcpy(ptr %src) sanitize_thread {
; CHECK-LABEL: @pointee_from_memcpy
; CHECK:       call void @__tsan_write4(ptr %p)
entry:
  %s = alloca { ptr, ptr }, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr %s, ptr %src, i64 16, i1 false)
  %f = getelementptr inbounds { ptr, ptr }, ptr %s, i64 0, i32 1
  %p = load ptr, ptr %f, align 8
  store i32 0, ptr %p, align 4
  ret void
}

define void @pointee_known_local() sanitize_thread {
; CHECK-LABEL: @pointee_known_local
; CHECK-NOT:   call void @__tsan_write4
; CHECK:       ret void
entry:
  %y = alloca i32, align 4
  %slot = alloca ptr, align 8
  store ptr %y, ptr %slot, align 8
  %p = load ptr, ptr %slot, align 8
  store i32 0, ptr %p, align 4
  ret void
}
