; Only a call to a function with a standard lock name is an acquisition. The
; recogniser matched substrings, so any function whose name merely contains
; "lock" -- memblock_get, block_signals, mlock -- was one, and two threads that
; both called it around their writes to @G were "consistently protected". @H
; is the control, under a real pthread mutex.

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --implicit-check-not='* G'

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@pool = global { i64, i64 } zeroinitializer, align 8
@m = global i64 0, align 8
@G = internal global i32 0, align 4
@H = internal global i32 0, align 4
declare ptr @memblock_get(ptr)
declare void @block_signals(ptr)
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @w1(ptr %a) {
  %b = call ptr @memblock_get(ptr @pool)
  store i32 1, ptr @G, align 4
  ret ptr null
}

define internal ptr @w2(ptr %a) {
  call void @block_signals(ptr @pool)
  store i32 2, ptr @G, align 4
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
