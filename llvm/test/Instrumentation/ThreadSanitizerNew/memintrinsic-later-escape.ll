; A memcpy between locals is lowered to an intercepted __tsan_memcpy so the
; runtime sees the accesses; when both locals provably never escape the
; interceptor is toggled off around the intrinsic instead. The toggle used
; the bare per-point escape query: a struct filled by memcpy and then
; handed to an opaque call in a later block -- a timeval passed to
; select(), a buffer passed to write() -- had not escaped at the memcpy,
; so its interceptor was skipped although the callee, and whatever thread
; it hands the pointer to, reads it. The rule is now the sound
; flow-sensitive one: every escape reachable after the access must be
; release-like (thread creation is; an opaque call is not).
; @escapes_later keeps the interceptor; @never_escapes and
; @published_by_create toggle it off.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

%struct.tv = type { i64, i64 }
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
declare i32 @select(i32, ptr, ptr, ptr, ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)
declare ptr @worker(ptr)

; CHECK-LABEL: define void @escapes_later
; CHECK-NOT: store i1 false, ptr @InterceptorEnabled
; CHECK: @__tsan_memcpy(ptr %tv, ptr %src, i64 16)
; CHECK-NOT: store i1 false, ptr @InterceptorEnabled
; CHECK: @select(
define void @escapes_later() sanitize_thread {
entry:
  %tv = alloca %struct.tv, align 8
  %src = alloca %struct.tv, align 8
  store i64 1, ptr %src, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr %tv, ptr %src, i64 16, i1 false)
  br label %use
use:
  %r = call i32 @select(i32 0, ptr null, ptr null, ptr null, ptr %tv)
  ret void
}

; CHECK-LABEL: define i64 @never_escapes
; CHECK: store i1 false, ptr @InterceptorEnabled
; CHECK: call void @llvm.memcpy
; CHECK: store i1 %{{.*}}, ptr @InterceptorEnabled
define i64 @never_escapes() sanitize_thread {
  %tv = alloca %struct.tv, align 8
  %src = alloca %struct.tv, align 8
  store i64 1, ptr %src, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr %tv, ptr %src, i64 16, i1 false)
  %v = load i64, ptr %tv, align 8
  ret i64 %v
}

; CHECK-LABEL: define void @published_by_create
; CHECK: store i1 false, ptr @InterceptorEnabled
; CHECK: call void @llvm.memcpy
; CHECK: store i1 %{{.*}}, ptr @InterceptorEnabled
; CHECK: @pthread_create(
define void @published_by_create() sanitize_thread {
entry:
  %t = alloca i64, align 8
  %tv = alloca %struct.tv, align 8
  %src = alloca %struct.tv, align 8
  store i64 1, ptr %src, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr %tv, ptr %src, i64 16, i1 false)
  br label %pub
pub:
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr %tv)
  ret void
}
