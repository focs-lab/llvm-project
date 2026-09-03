; The __tsan_mutex_* family are annotations whose effect depends on flags passed
; beside them -- __tsan_mutex_try_lock, __tsan_mutex_read_lock, and
; __tsan_mutex_try_lock_failed on the matching post_lock. Their names all
; contain "lock", so pattern matching took every one for an acquisition,
; pre_unlock included, and a try-lock that *failed* left the mutex held for the
; rest of the function.
;
; @control is here so the test can tell "annotations hold nothing" apart from
; "the analysis gave up on this module".

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --implicit-check-not='* via_annotation'

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@mtx = global i64 0, align 8
@custom = global i64 0, align 8
@control = internal global i32 0, align 4
@via_annotation = internal global i32 0, align 4

declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)
declare void @__tsan_mutex_pre_lock(ptr, i32)
declare void @__tsan_mutex_post_lock(ptr, i32, i32)
declare void @__tsan_mutex_pre_unlock(ptr, i32)
declare void @__tsan_mutex_post_unlock(ptr, i32)

define internal ptr @worker(ptr %arg) nounwind {
entry:
  %l = call i32 @pthread_mutex_lock(ptr @mtx)
  store i32 1, ptr @control, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @mtx)

  ; A failing try-lock, annotated. Nothing is held afterwards.
  call void @__tsan_mutex_pre_lock(ptr @custom, i32 1)
  call void @__tsan_mutex_post_lock(ptr @custom, i32 3, i32 0)
  store i32 1, ptr @via_annotation, align 4
  call void @__tsan_mutex_pre_unlock(ptr @custom, i32 0)
  call void @__tsan_mutex_post_unlock(ptr @custom, i32 0)
  ret ptr null
}

define i32 @main() nounwind {
entry:
  %t = alloca i64, align 8
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr null)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK: * control
