; RUN: opt < %s -passes=tsan -tsan-use-dominance-analysis=true -tsan-use-loop-peeling=true -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@GlobalVar = global i32 0, align 4

define void @test_loop_peeling(i32 %n) sanitize_thread {
entry:
  %cmp1 = icmp sgt i32 %n, 0
  br i1 %cmp1, label %loop.header, label %exit

loop.header:
  %i = phi i32 [ 0, %entry ], [ %inc, %loop.body ]
  br label %loop.body

loop.body:
  ; This access should be checked in the peeled block, but NOT in the loop body
  store i32 42, ptr @GlobalVar, align 4

  %inc = add i32 %i, 1
  %exitcond = icmp eq i32 %inc, %n
  br i1 %exitcond, label %exit, label %loop.header

exit:
  ret void
}

; Check that there is a Peeled iteration (before the main loop)
; CHECK-LABEL: @test_loop_peeling

; Search for the first store (in the peeled part) and instrumentation before it
; CHECK: loop.body.peel:
; CHECK: __tsan_write4
; CHECK-NEXT: store i32 42, ptr @GlobalVar, align 4

; 4. IMPORTANT: In the loop body there should be a store, but there should NOT be __tsan_write before it!
; CHECK: loop.body:
; CHECK-NOT: __tsan_write4
; CHECK: store i32 42, ptr @GlobalVar, align 4