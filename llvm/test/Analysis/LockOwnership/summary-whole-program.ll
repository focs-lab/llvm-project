; Whole-program lock ownership. @counter is external (defined in unit a,
; also written in unit b), guarded everywhere by the external mutex @m.
; Unit b alone cannot judge it: another unit could write it unlocked, and an
; opaque call could release @m. The linked program, analysed with
; -tsan-whole-program, sees every access and every releaser, and its
; summary lets unit b treat @counter as protected. @unguarded is written
; without the lock in unit a and never appears.

; RUN: rm -rf %t && mkdir -p %t && split-file %s %t
; RUN: llvm-link -S %t/a.ll %t/b.ll -o %t/linked.ll
; RUN: opt < %t/linked.ll -passes='print<lock-ownership>' -disable-output -tsan-use-analysis-summaries -tsan-whole-program -tsan-summary-dir=%t/sum -tsan-summary-id=t1 2>&1 | FileCheck %s --check-prefix=LINKED --implicit-check-not='* unguarded'
; RUN: opt < %t/b.ll -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --check-prefix=ALONE --implicit-check-not='* counter'
; RUN: opt < %t/b.ll -passes='print<lock-ownership>' -disable-output -tsan-use-analysis-summaries -tsan-summary-dir=%t/sum -tsan-summary-id=t1 2>&1 | FileCheck %s --check-prefix=SEEDED --implicit-check-not='* unguarded'

; LINKED: * counter
; ALONE: Protected Global Variables
; SEEDED: * counter

;--- a.ll
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
@m = global i64 0, align 8
@counter = global i32 0, align 4
@unguarded = global i32 0, align 4
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)
declare ptr @worker_b(ptr)
define internal ptr @worker_a(ptr %p) {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  store i32 1, ptr @counter, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  store i32 1, ptr @unguarded, align 4
  ret ptr null
}
define i32 @main() {
  %t = alloca i64, align 8
  %c1 = call i32 @pthread_create(ptr %t, ptr null, ptr @worker_a, ptr null)
  %c2 = call i32 @pthread_create(ptr %t, ptr null, ptr @worker_b, ptr null)
  ret i32 0
}

;--- b.ll
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
@m = external global i64, align 8
@counter = external global i32, align 4
@unguarded = external global i32, align 4
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
define ptr @worker_b(ptr %p) {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  store i32 2, ptr @counter, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  store i32 2, ptr @unguarded, align 4
  ret ptr null
}
