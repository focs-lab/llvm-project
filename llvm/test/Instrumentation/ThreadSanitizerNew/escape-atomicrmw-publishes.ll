; A pointer handed to an atomicrmw as the value operand is published: the
; exchange stores it where another thread can load it. The local's later
; write must stay instrumented. The control never leaves the function.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@slot = global ptr null, align 8

; CHECK-LABEL: @published
; CHECK: call void @__tsan_write4(ptr %l)
define void @published() sanitize_thread {
  %l = alloca i32, align 4
  %old = atomicrmw xchg ptr @slot, ptr %l seq_cst
  store i32 1, ptr %l, align 4
  ret void
}

; CHECK-LABEL: @kept_private
; CHECK-NOT: call void @__tsan_write4
; CHECK: ret void
define void @kept_private() sanitize_thread {
  %l = alloca i32, align 4
  store i32 1, ptr %l, align 4
  ret void
}
