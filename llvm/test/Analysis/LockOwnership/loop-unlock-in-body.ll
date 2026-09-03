; A must-held lockset is the intersection over every path to the access. In
; this loop the header's access runs locked on the first iteration and
; unlocked on every later one (the body unlocks and loops back). The analysis
; visited the header before the back edge existed in its tables, recorded the
; access under the lock, and never revised it. @H is the control: the unlock
; sits after the loop, so every iteration holds the lock.

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --implicit-check-not='* G'

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@m = global i64 0, align 8
@G = internal global i32 0, align 4
@H = internal global i32 0, align 4
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @unlock_in_body(ptr %a) {
entry:
  %l = call i32 @pthread_mutex_lock(ptr @m)
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %n, %body ]
  store i32 1, ptr @G, align 4
  %c = icmp slt i32 %i, 10
  br i1 %c, label %body, label %exit
body:
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  %n = add i32 %i, 1
  br label %header
exit:
  ret ptr null
}

define internal ptr @unlock_after_loop(ptr %a) {
entry:
  %l = call i32 @pthread_mutex_lock(ptr @m)
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %n, %body ]
  store i32 1, ptr @H, align 4
  %c = icmp slt i32 %i, 10
  br i1 %c, label %body, label %exit
body:
  %n = add i32 %i, 1
  br label %header
exit:
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define internal ptr @other(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  store i32 2, ptr @G, align 4
  store i32 2, ptr @H, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define i32 @main() {
  %t = alloca i64, align 8
  %c1 = call i32 @pthread_create(ptr %t, ptr null, ptr @unlock_in_body, ptr null)
  %c2 = call i32 @pthread_create(ptr %t, ptr null, ptr @unlock_after_loop, ptr null)
  %c3 = call i32 @pthread_create(ptr %t, ptr null, ptr @other, ptr null)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK-DAG: * H
