; Cover chains through a loop. In @chain the preheader store covers the
; first body store, which covers the second; both are removed and the
; preheader store is the surviving root of the chain. In @latch_unlock the
; back edge passes an unlock, so the path from the preheader store to the
; first body store on a second iteration is not clean and that store is
; kept. The second body store sits on the same cycle: an access removed on
; a cycle has the whole cycle scanned, the unlock is on it, so it is kept
; as well (conservative, and what the argument for chains relies on).
; Dominance only: with post-dominance on, the first body store would also
; cover the preheader store, which is a different chain. Guard for the
; chain composition: what survives is exactly the root of
; each chain, never an access whose cover was itself removed.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-dom -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4
@m = global i64 0, align 8
declare i32 @pthread_mutex_unlock(ptr)

; CHECK-LABEL: @chain
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK-NOT: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @chain(i32 %n) sanitize_thread {
entry:
  store i32 0, ptr @x, align 4
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  store i32 1, ptr @x, align 4
  store i32 2, ptr @x, align 4
  %inc = add i32 %i, 1
  %c = icmp slt i32 %inc, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: @latch_unlock
; CHECK-COUNT-3: call void @__tsan_write4(ptr @x)
; CHECK-NOT: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @latch_unlock(i32 %n) sanitize_thread {
entry:
  store i32 0, ptr @x, align 4
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %latch ]
  store i32 1, ptr @x, align 4
  store i32 2, ptr @x, align 4
  br label %latch
latch:
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  %inc = add i32 %i, 1
  %c = icmp slt i32 %inc, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}
