; The must-lockset meet over two paths that acquire the same mutex at
; different call sites: the lock is held on both, so the access after the
; join is protected (the recorded acquisition site is one of the two, which
; matters only for reporting). @G is written under m in both workers.

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@m = global i64 0, align 8
@G = internal global i32 0, align 4
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @w1(ptr %a) {
entry:
  %c = icmp eq ptr %a, null
  br i1 %c, label %A, label %B
A:
  %l1 = call i32 @pthread_mutex_lock(ptr @m)
  br label %join
B:
  %l2 = call i32 @pthread_mutex_lock(ptr @m)
  br label %join
join:
  store i32 1, ptr @G, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define internal ptr @w2(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  store i32 2, ptr @G, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

define i32 @main() {
  %t = alloca i64, align 8
  %c1 = call i32 @pthread_create(ptr %t, ptr null, ptr @w1, ptr null)
  %c2 = call i32 @pthread_create(ptr %t, ptr null, ptr @w2, ptr null)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK: * G
