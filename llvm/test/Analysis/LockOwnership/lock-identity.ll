; What counts as "the same mutex". A lock is known only when it is a global or
; a constant offset into one, and two such references to the same offset are
; the same lock however they are spelled. Everything else -- a mutex reached
; through an argument or a loaded pointer -- is unknown and protects nothing.
;
; Before this, a lock was identified by its underlying object, so both fields
; of @S were one lock, both stripes of @locks were one lock, and a pointer
; argument was a lock. Each let a real race go uninstrumented. The globals
; expected in the protected list are the controls: without them a fix that
; simply stopped concluding anything would satisfy the CHECK-NOTs.
;
; @reacquire also covers the crash this replaced: a release with no matching
; acquisition in the function, followed by a lock/unlock pair of the same
; mutex, asserted in handleUnlock (memcached extstore.c, MySQL thr_mutex.cc).

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --implicit-check-not='* two_fields' --implicit-check-not='* stripe_diff' --implicit-check-not='* via_arg' --implicit-check-not='* via_load'

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

; Two mutexes as fields of one global; two mutexes as elements of one array.
@S = global { i64, i64 } zeroinitializer, align 8
@locks = global [2 x i64] zeroinitializer, align 8
@mptr = global ptr @S, align 8

@same_field  = internal global i32 0, align 4  ; both threads under S.f0 (spelled two ways)
@two_fields  = internal global i32 0, align 4  ; one thread under S.f0, the other under S.f1
@stripe_same = internal global i32 0, align 4  ; both under locks[0] (spelled two ways)
@stripe_diff = internal global i32 0, align 4  ; locks[0] vs locks[1]
@via_arg     = internal global i32 0, align 4  ; mutex arrives as a pointer argument
@via_load    = internal global i32 0, align 4  ; mutex pointer is loaded
@reacquire   = internal global i32 0, align 4  ; unlock-without-lock, then a proper pair

declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @worker1(ptr %m) nounwind {
entry:
  ; S.f0 via a GEP instruction.
  %s0 = getelementptr inbounds { i64, i64 }, ptr @S, i64 0, i32 0
  %l1 = call i32 @pthread_mutex_lock(ptr %s0)
  store i32 1, ptr @same_field, align 4
  store i32 1, ptr @two_fields, align 4
  %u1 = call i32 @pthread_mutex_unlock(ptr %s0)

  ; locks[0] via a constant expression.
  %l2 = call i32 @pthread_mutex_lock(ptr getelementptr inbounds ([2 x i64], ptr @locks, i64 0, i64 0))
  store i32 1, ptr @stripe_same, align 4
  store i32 1, ptr @stripe_diff, align 4
  %u2 = call i32 @pthread_mutex_unlock(ptr getelementptr inbounds ([2 x i64], ptr @locks, i64 0, i64 0))

  ; The mutex is whatever the caller passed.
  %l3 = call i32 @pthread_mutex_lock(ptr %m)
  store i32 1, ptr @via_arg, align 4
  %u3 = call i32 @pthread_mutex_unlock(ptr %m)

  ; The mutex is whatever @mptr holds.
  %p = load ptr, ptr @mptr, align 8
  %l4 = call i32 @pthread_mutex_lock(ptr %p)
  store i32 1, ptr @via_load, align 4
  %u4 = call i32 @pthread_mutex_unlock(ptr %p)

  ; Release of S.f0 with no acquisition here, then a well-formed pair.
  %u5 = call i32 @pthread_mutex_unlock(ptr %s0)
  %l6 = call i32 @pthread_mutex_lock(ptr %s0)
  store i32 1, ptr @reacquire, align 4
  %u6 = call i32 @pthread_mutex_unlock(ptr %s0)
  ret ptr null
}

define internal ptr @worker2(ptr %m) nounwind {
entry:
  ; S.f0 spelled as a byte offset -- must be the same lock as worker1's.
  %l1 = call i32 @pthread_mutex_lock(ptr @S)
  store i32 2, ptr @same_field, align 4
  %u1 = call i32 @pthread_mutex_unlock(ptr @S)

  ; S.f1: a different mutex in the same object.
  %s1 = getelementptr inbounds i8, ptr @S, i64 8
  %l2 = call i32 @pthread_mutex_lock(ptr %s1)
  store i32 2, ptr @two_fields, align 4
  %u2 = call i32 @pthread_mutex_unlock(ptr %s1)

  ; locks[0] via a GEP instruction -- same lock as worker1's constant.
  %k0 = getelementptr inbounds [2 x i64], ptr @locks, i64 0, i64 0
  %l3 = call i32 @pthread_mutex_lock(ptr %k0)
  store i32 2, ptr @stripe_same, align 4
  %u3 = call i32 @pthread_mutex_unlock(ptr %k0)

  ; locks[1]: a different stripe.
  %k1 = getelementptr inbounds [2 x i64], ptr @locks, i64 0, i64 1
  %l4 = call i32 @pthread_mutex_lock(ptr %k1)
  store i32 2, ptr @stripe_diff, align 4
  %u4 = call i32 @pthread_mutex_unlock(ptr %k1)

  %l5 = call i32 @pthread_mutex_lock(ptr %m)
  store i32 2, ptr @via_arg, align 4
  %u5 = call i32 @pthread_mutex_unlock(ptr %m)

  %p = load ptr, ptr @mptr, align 8
  %l6 = call i32 @pthread_mutex_lock(ptr %p)
  store i32 2, ptr @via_load, align 4
  %u6 = call i32 @pthread_mutex_unlock(ptr %p)

  %l7 = call i32 @pthread_mutex_lock(ptr @S)
  store i32 2, ptr @reacquire, align 4
  %u7 = call i32 @pthread_mutex_unlock(ptr @S)
  ret ptr null
}

define i32 @main() nounwind {
entry:
  %t1 = alloca i64, align 8
  %t2 = alloca i64, align 8
  ; Both workers really do get the same mutex; the analysis must not assume it.
  %c1 = call i32 @pthread_create(ptr %t1, ptr null, ptr @worker1, ptr @S)
  %c2 = call i32 @pthread_create(ptr %t2, ptr null, ptr @worker2, ptr @S)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK-DAG: * same_field
; CHECK-DAG: * stripe_same
; CHECK-DAG: * reacquire
