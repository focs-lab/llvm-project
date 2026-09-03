; Dominance elimination may only drop an access when no synchronization can
; occur on any path from its cover to it -- including the path from the access
; back to itself, when it sits in a loop, and including synchronization hidden
; behind a call.
;
; These run through the module pass so that SyncFreeInfo exists. Running the
; function pass alone takes a different branch in the sync classification and
; would pass without exercising any of this.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-dom -S | FileCheck %s
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -S | FileCheck %s --check-prefix=NODOM

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g = global i32 0, align 4
@m = global i64 0, align 8

declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)

; A body-less external function: it may lock, unlock, or start a thread, and
; nothing in this module can prove otherwise.
declare void @opaque_external()

; A local function with no synchronization in it at all.
define internal void @quiet_local() nounwind {
  ret void
}

; Loop-free, but it releases a lock. Being loop-free says nothing about
; whether it synchronizes.
define internal void @loop_free_unlock() nounwind {
  %r = call i32 @pthread_mutex_unlock(ptr @m)
  ret void
}

;--- A call we cannot see into blocks elimination.
define void @path_opaque_external() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @path_opaque_external
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       call void @opaque_external()
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @path_opaque_external
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
entry:
  store i32 1, ptr @g, align 4
  call void @opaque_external()
  store i32 2, ptr @g, align 4
  ret void
}

;--- A local callee we can prove quiet does not block it. This is the positive
;--- control for the case above: without it, the test would pass just as well
;--- if every call blocked elimination.
define void @path_quiet_local_call() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @path_quiet_local_call
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       call void @quiet_local()
; CHECK-NOT:   call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @path_quiet_local_call
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
entry:
  store i32 1, ptr @g, align 4
  call void @quiet_local()
  store i32 2, ptr @g, align 4
  ret void
}

;--- A release behind a loop-free wrapper blocks elimination just as an inline
;--- release does. Termination and synchronization are different questions.
define void @path_release_behind_wrapper() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @path_release_behind_wrapper
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       call void @loop_free_unlock()
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @path_release_behind_wrapper
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
entry:
  store i32 1, ptr @g, align 4
  call void @loop_free_unlock()
  store i32 2, ptr @g, align 4
  ret void
}

;--- An inline release blocks elimination ...
define void @path_release_inline() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @path_release_inline
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       call i32 @pthread_mutex_unlock
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @path_release_inline
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
entry:
  store i32 1, ptr @g, align 4
  %r = call i32 @pthread_mutex_lock(ptr @m)
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  store i32 2, ptr @g, align 4
  ret void
}

;--- ... while an acquire on its own does not. An acquire can only order a
;--- remote event before the later access, which cannot create a race the
;--- earlier access would have missed.
define void @path_acquire_only() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @path_acquire_only
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       call i32 @pthread_mutex_lock
; CHECK-NOT:   call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @path_acquire_only
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
entry:
  store i32 1, ptr @g, align 4
  %r = call i32 @pthread_mutex_lock(ptr @m)
  store i32 2, ptr @g, align 4
  ret void
}

;--- The loop back-edge. The cover sits outside the loop and runs once; the
;--- access inside runs every iteration, and consecutive iterations are
;--- separated by the unlock in the latch. Only the first iteration is covered,
;--- so nothing may be removed.
define void @loop_backedge_releases(i32 %n) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @loop_backedge_releases
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       call void @__tsan_write4(ptr @g)
; NODOM-LABEL: @loop_backedge_releases
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
entry:
  store i32 1, ptr @g, align 4
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
  %cmp = icmp slt i32 %i, %n
  br i1 %cmp, label %body, label %exit

body:
  store i32 2, ptr @g, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  br label %latch

latch:
  %i.next = add i32 %i, 1
  br label %header

exit:
  ret void
}

;--- The same loop shape with a quiet latch: here the in-loop access really is
;--- covered on every iteration, and is removed. Without this case the test
;--- above would also pass if loops simply disabled the optimization.
define void @loop_backedge_quiet(i32 %n) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @loop_backedge_quiet
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK-NOT:   call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @loop_backedge_quiet
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
entry:
  store i32 1, ptr @g, align 4
  br label %header

header:
  %i = phi i32 [ 0, %entry ], [ %i.next, %latch ]
  %cmp = icmp slt i32 %i, %n
  br i1 %cmp, label %body, label %exit

body:
  store i32 2, ptr @g, align 4
  br label %latch

latch:
  %i.next = add i32 %i, 1
  br label %header

exit:
  ret void
}

;--- A branch that diverges and never reaches the covered access must not veto
;--- the elimination: nothing on it can run between the two accesses.
define void @diverging_branch_does_not_veto(i1 %c) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @diverging_branch_does_not_veto
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       br i1
; CHECK-NOT:   call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @diverging_branch_does_not_veto
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
entry:
  store i32 1, ptr @g, align 4
  br i1 %c, label %join, label %other

other:
  ; This block synchronizes, but it leaves the function; control never gets
  ; from here to the second access.
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret void

join:
  store i32 2, ptr @g, align 4
  ret void
}
