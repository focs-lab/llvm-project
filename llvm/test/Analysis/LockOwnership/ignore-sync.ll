; Inside an AnnotateIgnoreSyncBegin region a mutex still excludes but no longer
; establishes happens-before, so two threads holding it genuinely race and TSan
; is expected to say so. No static lockset can see that, and concluding a global
; is protected would suppress exactly the report the annotation exists to
; produce -- so in a module that uses it, nothing is concluded.
;
; The shape below is the one locks.ll proves *is* reported as protected when the
; annotation is absent; that file is this test's control.

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@mtx = global i64 0, align 8
@under_mutex = global i32 0, align 4

declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)
declare void @AnnotateIgnoreSyncBegin(ptr, i32)
declare void @AnnotateIgnoreSyncEnd(ptr, i32)

define internal ptr @worker(ptr %arg) nounwind {
entry:
  call void @AnnotateIgnoreSyncBegin(ptr null, i32 0)
  %l = call i32 @pthread_mutex_lock(ptr @mtx)
  store i32 1, ptr @under_mutex, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @mtx)
  call void @AnnotateIgnoreSyncEnd(ptr null, i32 0)
  ret ptr null
}

define i32 @main() nounwind {
entry:
  %t = alloca i64, align 8
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr null)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK: (empty)
