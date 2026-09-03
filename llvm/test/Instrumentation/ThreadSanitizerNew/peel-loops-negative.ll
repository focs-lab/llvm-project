; Loop peeling is worth it only when an access in the loop has a loop-
; invariant address, so that the peeled first iteration can cover the rest
; under dominance. A loop that only walks an array, or only touches
; volatile locations, is left alone; the control with an invariant global
; store is peeled and its body's access appears twice.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis -tsan-use-loop-peeling -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g = global i32 0, align 4
@v = global i32 0, align 4

; CHECK-LABEL: @walk
; CHECK-COUNT-1: call void @__tsan_write4(
; CHECK-NOT: call void @__tsan_write4(
; CHECK: ret void
define void @walk(ptr %a, i32 %n) sanitize_thread {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %p = getelementptr inbounds i32, ptr %a, i32 %i
  store i32 0, ptr %p, align 4
  %inc = add i32 %i, 1
  %c = icmp slt i32 %inc, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: @volatile_only
; CHECK-COUNT-1: call void @__tsan_write4(ptr @v)
; CHECK-NOT: call void @__tsan_write4(ptr @v)
; CHECK: ret void
define void @volatile_only(i32 %n) sanitize_thread {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  store volatile i32 %i, ptr @v, align 4
  %inc = add i32 %i, 1
  %c = icmp slt i32 %inc, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

; CHECK-LABEL: @invariant
; CHECK: call void @__tsan_write4(ptr @g)
; CHECK: ret void
define void @invariant(i32 %n) sanitize_thread {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  store i32 %i, ptr @g, align 4
  %inc = add i32 %i, 1
  %c = icmp slt i32 %inc, %n
  br i1 %c, label %loop, label %exit
exit:
  ret void
}
