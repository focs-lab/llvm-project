; Every name on the known-thread-creator list makes its caller a creator:
; pthread_create's aliases, C11 thrd_create, the OpenMP fork entry points
; and TSan's own fiber creation.

; RUN: opt < %s -passes='print<single-threaded>' -disable-output 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

declare i32 @__pthread_create_2_1(ptr, ptr, ptr, ptr)
declare i32 @thrd_create(ptr, ptr, ptr)
declare void @__kmpc_fork_call(ptr, i32, ptr, ...)
declare ptr @__tsan_create_fiber(i32)
declare ptr @w(ptr)
define internal void @c1() {
  %t = alloca i64
  %r = call i32 @__pthread_create_2_1(ptr %t, ptr null, ptr @w, ptr null)
  ret void
}
define internal void @c2() {
  %t = alloca i64
  %r = call i32 @thrd_create(ptr %t, ptr @w, ptr null)
  ret void
}
define internal void @c3() {
  call void (ptr, i32, ptr, ...) @__kmpc_fork_call(ptr null, i32 0, ptr @w)
  ret void
}
define internal void @c4() {
  %f = call ptr @__tsan_create_fiber(i32 0)
  ret void
}
define i32 @main() {
  call void @c1()
  call void @c2()
  call void @c3()
  call void @c4()
  ret i32 0
}

; CHECK-LABEL: Thread Creator Functions
; CHECK-DAG: c1
; CHECK-DAG: c2
; CHECK-DAG: c3
; CHECK-DAG: c4
; CHECK-LABEL: Multi-Threaded Functions
