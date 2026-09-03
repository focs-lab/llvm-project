; Which acquisitions actually hold something, and which globals that makes
; consistently protected. Every case here answered wrongly at some point, and
; each wrong answer let the instrumentation pass drop a real access.
;
; The two globals expected in the list are the controls: without them a fix
; that simply stopped concluding anything would satisfy the CHECK-NOTs.

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --implicit-check-not='* after_unlock' --implicit-check-not='* under_rdlock' --implicit-check-not='* after_trylock'

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@mtx = global i64 0, align 8
@rw = global i64 0, align 8

@under_mutex = internal global i32 0, align 4   ; held by a plain mutex
@under_wrlock = internal global i32 0, align 4  ; held by an rwlock writer
@after_unlock = internal global i32 0, align 4  ; touched once the lock is released
@under_rdlock = internal global i32 0, align 4  ; only ever under a reader lock
@after_trylock = internal global i32 0, align 4 ; touched after a try-lock, which may fail

declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_mutex_trylock(ptr)
declare i32 @pthread_rwlock_wrlock(ptr)
declare i32 @pthread_rwlock_rdlock(ptr)
declare i32 @pthread_rwlock_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @worker(ptr %arg) nounwind {
entry:
  ; Held for the whole access: protected.
  %l1 = call i32 @pthread_mutex_lock(ptr @mtx)
  store i32 1, ptr @under_mutex, align 4
  %u1 = call i32 @pthread_mutex_unlock(ptr @mtx)

  ; A writer lock excludes everyone, so this one is protected too. It was not
  ; recognised while the shared-lock patterns included a bare "rlock", which
  ; pthread_rwlock_w[rlock] contains.
  %l2 = call i32 @pthread_rwlock_wrlock(ptr @rw)
  store i32 1, ptr @under_wrlock, align 4
  %u2 = call i32 @pthread_rwlock_unlock(ptr @rw)

  ; Released before the access. Reading an unlock as an acquisition -- every
  ; unlock name contains "lock" -- left the mutex held for the rest of the
  ; function and made this look protected.
  %l3 = call i32 @pthread_mutex_lock(ptr @mtx)
  %u3 = call i32 @pthread_mutex_unlock(ptr @mtx)
  store i32 1, ptr @after_unlock, align 4

  ; A reader lock does not exclude other readers, so a write under one is not
  ; protected by it.
  %l4 = call i32 @pthread_rwlock_rdlock(ptr @rw)
  store i32 1, ptr @under_rdlock, align 4
  %u4 = call i32 @pthread_rwlock_unlock(ptr @rw)

  ; A try-lock reports whether it succeeded; on the failing path nothing is
  ; held.
  %t = call i32 @pthread_mutex_trylock(ptr @mtx)
  store i32 1, ptr @after_trylock, align 4
  ret ptr null
}

define i32 @main() nounwind {
entry:
  %t = alloca i64, align 8
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr null)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK-DAG: * under_mutex
; CHECK-DAG: * under_wrlock
