; RUN: opt < %s -passes=tsan --tsan-use-dominance-analysis -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@GV1 = global i32 0, align 4
@mutex = global i64 0, align 8

; ---
; TEST 2.1: Pre-dominance, Success (R -> lock -> R in branches)
; An access in the entry block dominates accesses in both branches. The path contains a lock.
; Expected: Instrumentation in both branches should be removed.

define void @predom_diamond_acquire_rr(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_diamond_acquire_rr
  ; access_A, the dominator, is instrumented.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val1 = load i32, ptr @GV1, align 4
  %val1 = load i32, ptr @GV1, align 4
  ; CHECK:      call i32 @pthread_mutex_lock(ptr @mutex)
  call i32 @pthread_mutex_lock(ptr @mutex)
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; access_B1, dominated, is NOT instrumented.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val_b1 = load i32, ptr @GV1, align 4
  %val_b1 = load i32, ptr @GV1, align 4
  br label %merge

branch2:
  ; CHECK:      branch2:
  ; access_B2, dominated, is NOT instrumented.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val_b2 = load i32, ptr @GV1, align 4
  %val_b2 = load i32, ptr @GV1, align 4
  br label %merge

merge:
  ret void
}

; ---
; TEST 2.2: Post-dominance, Success (W in branches -> unlock -> W)
; An access in the merge block post-dominates accesses in both branches. The path contains an unlock.
; Expected: Instrumentation in both branches should be removed.

define void @postdom_diamond_release_ww(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_diamond_release_ww
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; access_A1, post-dominated, is NOT instrumented.
  ; CHECK-NOT:  call void @__tsan_write4
  ; CHECK:      store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4
  ; CHECK:      call i32 @pthread_mutex_unlock(ptr @mutex)
  call i32 @pthread_mutex_unlock(ptr @mutex)
  br label %merge

branch2:
  ; CHECK:      branch2:
  ; access_A2, post-dominated, is NOT instrumented.
  ; CHECK-NOT:  call void @__tsan_write4
  ; CHECK:      store i32 2, ptr @GV1, align 4
  store i32 2, ptr @GV1, align 4
  ; CHECK:      call i32 @pthread_mutex_unlock(ptr @mutex)
  call i32 @pthread_mutex_unlock(ptr @mutex)
  br label %merge

merge:
  ; CHECK:      merge:
  ; access_B, the post-dominator, is instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 3, ptr @GV1, align 4
  store i32 3, ptr @GV1, align 4
  ret void
}

; ---
; TEST 2.3: Pre-dominance, Veto (W -> lock -> R in branches)
; Expected: The dominator and the dominated accesses in branches must be instrumented.

define void @predom_diamond_acquire_wr_veto(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_diamond_acquire_wr_veto
  ; access_A, the dominator, is instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4
  ; CHECK:      call i32 @pthread_mutex_lock(ptr @mutex)
  call i32 @pthread_mutex_lock(ptr @mutex)
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; access_B1, dominated, but the optimization is vetoed.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK:      %val_b1 = load i32, ptr @GV1, align 4
  %val_b1 = load i32, ptr @GV1, align 4
  br label %merge

branch2:
  ; CHECK:      branch2:
  ; access_B2, dominated, but the optimization is vetoed.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK:      %val_b2 = load i32, ptr @GV1, align 4
  %val_b2 = load i32, ptr @GV1, align 4
  br label %merge

merge:
  ret void
}

; Declarations of library functions that TSan recognizes.
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)