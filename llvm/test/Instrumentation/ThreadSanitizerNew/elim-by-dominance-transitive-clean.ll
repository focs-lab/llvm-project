; Interprocedural sync-freedom. @mid calls @leaf, both plain arithmetic, so a
; call to @mid between two stores to the same global synchronizes nothing and
; the second store is redundant. The sync-free summary used to consult a
; static pointer to itself while it was being built -- null on the first
; module -- so every call to a defined function looked synchronizing and only
; leaf functions were ever sync-free. Without the module pass there is no
; summary and the call is opaque: both stores stay (FUNC).

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis -S | FileCheck %s
; RUN: opt < %s -passes='function(tsan)' -tsan-use-dominance-analysis -S | FileCheck %s --check-prefix=FUNC

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4

define internal i32 @leaf(i32 %a) {
  %r = add i32 %a, 1
  ret i32 %r
}

define internal i32 @mid(i32 %a) {
  %r = call i32 @leaf(i32 %a)
  ret i32 %r
}

; CHECK-LABEL: @f
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK-NOT: call void @__tsan_write4(ptr @x)
; CHECK: ret void
; FUNC-LABEL: @f
; FUNC-COUNT-2: call void @__tsan_write4(ptr @x)
; FUNC: ret void
define void @f(i32 %a) sanitize_thread {
  store i32 1, ptr @x, align 4
  %r = call i32 @mid(i32 %a)
  store i32 %r, ptr @x, align 4
  ret void
}
