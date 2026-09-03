; An access whose base pointer the object walk cannot identify must keep its
; instrumentation. The walk clears its result and reports failure in that case,
; but the caller discarded the status and read the empty list as "nothing
; escapes", so every such access lost its check.
;
; The second function is what makes the first meaningful: escape analysis must
; still be doing its job, or a change that simply stopped eliding anything would
; satisfy the CHECK.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

; The address comes from arithmetic on an integer, so there is no object to
; reason about. Nothing is known, so nothing may be dropped.
define void @unidentified_base(i64 %bits) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @unidentified_base
; CHECK:       call void @__tsan_write4
; CHECK:       ret void
entry:
  %scaled = mul i64 %bits, 8
  %p = inttoptr i64 %scaled to ptr
  store i32 1, ptr %p, align 4
  ret void
}

; A local that never leaves the function: still elided.
define void @local_alloca() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @local_alloca
; CHECK-NOT:   call void @__tsan_write4
; CHECK:       ret void
entry:
  %x = alloca i32, align 4
  store i32 1, ptr %x, align 4
  ret void
}
