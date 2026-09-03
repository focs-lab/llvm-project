; The sync-free summary's loop part on its own: post-dominance may cross a
; call to a defined callee only if it is known to return, and a callee is
; known to return when it and everything it calls are free of loops. A
; loop-free callee is crossed and the earlier store elided; a callee with a
; loop (no willreturn anywhere) keeps both stores.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-postdom -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4

define internal i32 @straight(i32 %a) {
  %r = mul i32 %a, 3
  ret i32 %r
}

define internal i32 @spins(i32 %a) {
entry:
  br label %loop
loop:
  %i = phi i32 [ %a, %entry ], [ %d, %loop ]
  %d = sub i32 %i, 1
  %c = icmp ne i32 %d, 0
  br i1 %c, label %loop, label %exit
exit:
  ret i32 %d
}

; CHECK-LABEL: @through_straight
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK-NOT: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @through_straight(i32 %a) sanitize_thread {
  store i32 1, ptr @x, align 4
  %r = call i32 @straight(i32 %a)
  store i32 %r, ptr @x, align 4
  ret void
}

; CHECK-LABEL: @through_spins
; CHECK-COUNT-2: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @through_spins(i32 %a) sanitize_thread {
  store i32 1, ptr @x, align 4
  %r = call i32 @spins(i32 %a)
  store i32 %r, ptr @x, align 4
  ret void
}
