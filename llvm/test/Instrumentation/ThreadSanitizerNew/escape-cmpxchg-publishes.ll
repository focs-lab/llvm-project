; A pointer handed to another thread through a cmpxchg -- the way a lock-free
; list links a node -- has escaped exactly as if it had been stored. It was not:
; the reason bits for a cmpxchg value operand were assigned to a bitset too
; narrow to hold them and truncated to "no reason", so the object was recorded
; as escaped-for-no-reason, which every query reads as not escaped. The second
; function is the control: a cmpxchg *on* a local's own bytes publishes
; nothing.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@head = global ptr null, align 8

define void @published_by_cmpxchg() sanitize_thread {
; CHECK-LABEL: @published_by_cmpxchg
; CHECK:       call void @__tsan_write4(ptr %n)
entry:
  %n = alloca i32, align 4
  %c = cmpxchg ptr @head, ptr null, ptr %n seq_cst seq_cst
  store i32 1, ptr %n, align 4
  ret void
}

define void @cmpxchg_on_local() sanitize_thread {
; CHECK-LABEL: @cmpxchg_on_local
; CHECK-NOT:   call void @__tsan_write4(ptr %n)
; CHECK:       ret void
entry:
  %n = alloca i32, align 4
  %c = cmpxchg ptr %n, i32 0, i32 1 seq_cst seq_cst
  store i32 1, ptr %n, align 4
  ret void
}
