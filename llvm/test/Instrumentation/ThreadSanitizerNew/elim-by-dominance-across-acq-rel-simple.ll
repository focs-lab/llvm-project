; RUN: opt < %s -passes=tsan --tsan-use-dominance-analysis -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@GV1 = global i32 0, align 4
@mutex = global i64 0, align 8

; ---
; TEST 1.1: Pre-dominance, Success (R -> lock -> R)
; Expected: The second read's instrumentation should be removed.

define void @predom_simple_acquire_rr() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_simple_acquire_rr
  ; access_A, the dominator, is instrumented.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val1 = load i32, ptr @GV1, align 4
  %val1 = load i32, ptr @GV1, align 4

  ; TSan sees the lock, considers the path clear, but leaves the call as is.
  ; CHECK:      call i32 @pthread_mutex_lock(ptr @mutex)
  call i32 @pthread_mutex_lock(ptr @mutex)

  ; access_B, the dominated access, is NOT instrumented.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val2 = load i32, ptr @GV1, align 4
  %val2 = load i32, ptr @GV1, align 4
  ret void
}

; ---
; TEST 1.2: Pre-dominance, Veto (R -> lock -> W)
; Expected: Both accesses must be instrumented.

define void @predom_simple_acquire_wr_veto() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_simple_acquire_wr_veto
  ; access_A, the dominator, is instrumented.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4

  ; CHECK:      call i32 @pthread_mutex_lock(ptr @mutex)
  call i32 @pthread_mutex_lock(ptr @mutex)

  ; access_B, the dominated access, but the optimization is vetoed.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4
  ret void
}

; ---
; TEST 1.3: Post-dominance, Success (W -> unlock -> W)
; Expected: The first write's instrumentation should be removed.

define void @postdom_simple_release_ww() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_simple_release_ww
  ; access_A, the post-dominated access, is NOT instrumented.
  ; CHECK-NOT:  call void @__tsan_write4
  ; CHECK:      store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4

  ; CHECK:      call i32 @pthread_mutex_unlock(ptr @mutex)
  call i32 @pthread_mutex_unlock(ptr @mutex)

  ; access_B, the post-dominator, is instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 2, ptr @GV1, align 4
  store i32 2, ptr @GV1, align 4
  ret void
}

; ---
; TEST 1.4: Post-dominance, Veto (W -> unlock -> R)
; Expected: Both accesses must be instrumented.

define void @postdom_simple_release_rw_veto() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_simple_release_rw_veto
  ; access_A, the post-dominated access, but the optimization is vetoed.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4

  ; CHECK:      call i32 @pthread_mutex_unlock(ptr @mutex)
  call i32 @pthread_mutex_unlock(ptr @mutex)

  ; access_B, the post-dominator, is instrumented.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4
  ret void
}

; Declarations of library functions that TSan recognizes.
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)