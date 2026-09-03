; -tsan-ea-flow-insensitive selects the per-object notion of escape the paper's
; proposition states: an object that escapes anywhere in the function is
; escaped everywhere in it. The default is per program point and elides the
; write below (see the same shape in escape-adversarial.ll); under the flag it
; must stay. The unpublished local is the control: it must still be elided,
; or the flag would simply have turned escape analysis off.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -tsan-ea-flow-insensitive -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@sink = global ptr null, align 8

define void @published_in_later_block() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @published_in_later_block
; CHECK:       call void @__tsan_write4(ptr %x)
; CHECK:       call void @__tsan_write8(ptr @sink)
entry:
  %x = alloca i32, align 4
  store i32 1, ptr %x, align 4
  br label %later
later:
  store ptr %x, ptr @sink, align 8
  ret void
}

define void @never_published() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @never_published
; CHECK-NOT:   call void @__tsan_write4(ptr %x)
; CHECK:       ret void
entry:
  %x = alloca i32, align 4
  store i32 1, ptr %x, align 4
  br label %later
later:
  ret void
}
