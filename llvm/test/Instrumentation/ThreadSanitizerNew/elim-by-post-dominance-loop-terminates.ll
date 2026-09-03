; Post-dominance across a loop. The covering access after the loop is
; reached only if the loop terminates: with a computable trip count it
; does, and the earlier access is redundant; with an unknown exit it may
; spin forever, and the earlier access stays. -tsan-postdom-aggressive
; drops the termination requirement.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-postdom -S | FileCheck %s
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-postdom -tsan-postdom-aggressive -S | FileCheck %s --check-prefix=AGGR

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4
@y = global i32 0, align 4

; CHECK-LABEL: @counted
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK-NOT: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @counted() sanitize_thread {
entry:
  store i32 1, ptr @x, align 4
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop ]
  %v = load i32, ptr @y, align 4
  %inc = add i32 %i, 1
  %c = icmp slt i32 %inc, 10
  br i1 %c, label %loop, label %exit
exit:
  store i32 2, ptr @x, align 4
  ret void
}

; CHECK-LABEL: @unknown
; CHECK-COUNT-2: call void @__tsan_write4(ptr @x)
; CHECK: ret void
; AGGR-LABEL: @unknown
; AGGR: call void @__tsan_write4(ptr @x)
; AGGR-NOT: call void @__tsan_write4(ptr @x)
; AGGR: ret void
define void @unknown(ptr %flag) sanitize_thread {
entry:
  store i32 1, ptr @x, align 4
  br label %loop
loop:
  %v = load volatile i32, ptr %flag, align 4
  %c = icmp ne i32 %v, 0
  br i1 %c, label %loop, label %exit
exit:
  store i32 2, ptr @x, align 4
  ret void
}
