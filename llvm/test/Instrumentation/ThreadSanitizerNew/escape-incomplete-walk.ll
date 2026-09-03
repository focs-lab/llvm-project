; An address the object walk cannot resolve may be anything. Storing a local's
; pointer through it must escape the local; instead the transfer function
; skipped an operand whose walk came back incomplete, and the store vanished.
; The control stores through an integer-laundered pointer the walk CAN resolve
; (ptrtoint, add a constant, inttoptr): that is the same local, still private.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

define void @store_through_unknown(i64 %addr) sanitize_thread {
; CHECK-LABEL: @store_through_unknown
; CHECK:       call void @__tsan_write4(ptr %x)
entry:
  %x = alloca i32, align 4
  %d = inttoptr i64 %addr to ptr
  store ptr %x, ptr %d, align 8
  store i32 1, ptr %x, align 4
  ret void
}

define void @store_through_resolvable() sanitize_thread {
; CHECK-LABEL: @store_through_resolvable
; CHECK-NOT:   call void @__tsan_write4
; CHECK:       ret void
entry:
  %x = alloca { i32, i32 }, align 8
  %i = ptrtoint ptr %x to i64
  %j = add i64 %i, 4
  %p = inttoptr i64 %j to ptr
  store i32 1, ptr %p, align 4
  ret void
}
