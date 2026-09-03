; A timed acquisition returns without the lock on its timeout path, and
; that path is part of its contract; the runtime records the lock only when
; the call succeeds. Counting pthread_mutex_timedlock (and the timed and
; clock rwlock variants, and mtx_timedlock) as an unconditional acquisition
; made the write after it look protected on the timeout path. @T is written
; after timed locks only and is unprotected; @P after plain locks is the
; control.

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --implicit-check-not='* T'

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@m = global i64 0, align 8
@T = internal global i32 0, align 4
@P = internal global i32 0, align 4
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_timedlock(ptr, ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @w1(ptr %a) {
  %ts = alloca i64, align 8
  %l = call i32 @pthread_mutex_timedlock(ptr @m, ptr %ts)
  store i32 1, ptr @T, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define internal ptr @w2(ptr %a) {
  %ts = alloca i64, align 8
  %l = call i32 @pthread_mutex_timedlock(ptr @m, ptr %ts)
  store i32 2, ptr @T, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define internal ptr @w3(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  store i32 3, ptr @P, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define internal ptr @w4(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  store i32 4, ptr @P, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define i32 @main() {
  %t = alloca i64, align 8
  %c1 = call i32 @pthread_create(ptr %t, ptr null, ptr @w1, ptr null)
  %c2 = call i32 @pthread_create(ptr %t, ptr null, ptr @w2, ptr null)
  %c3 = call i32 @pthread_create(ptr %t, ptr null, ptr @w3, ptr null)
  %c4 = call i32 @pthread_create(ptr %t, ptr null, ptr @w4, ptr null)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK: * P
