; A callee that unlocks the caller's mutex ends the critical section at the
; call. The callee's exit state carried only acquisitions, so the caller kept
; believing the lock held and the unlocked write after the call was counted as
; protected. @H is the control: locked and unlocked in the same function.

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --implicit-check-not='* G'

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@m = global i64 0, align 8
@G = internal global i32 0, align 4
@H = internal global i32 0, align 4
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal void @release() {
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret void
}

define internal ptr @w1(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  call void @release()
  store i32 1, ptr @G, align 4
  ret ptr null
}

define internal ptr @w2(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  store i32 2, ptr @G, align 4
  store i32 2, ptr @H, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define internal ptr @w3(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  store i32 3, ptr @H, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define i32 @main() {
  %t = alloca i64, align 8
  %c1 = call i32 @pthread_create(ptr %t, ptr null, ptr @w1, ptr null)
  %c2 = call i32 @pthread_create(ptr %t, ptr null, ptr @w2, ptr null)
  %c3 = call i32 @pthread_create(ptr %t, ptr null, ptr @w3, ptr null)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK-DAG: * H
