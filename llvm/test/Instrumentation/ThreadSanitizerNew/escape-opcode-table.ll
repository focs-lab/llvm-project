; Every way a pointer value can leave the walk's sight is an escape. The
; classifier answered "no escape" for any opcode it had no case for -- with a
; comment wondering whether that was too aggressive. It was: an address turned
; into an integer and stored, or wrapped in an aggregate and returned, reaches
; other threads just as a stored pointer does. The control freezes a pointer
; and writes through it: freeze is transparent.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@isink = global i64 0, align 8

define void @ptrtoint_stored() sanitize_thread {
; CHECK-LABEL: @ptrtoint_stored
; CHECK:       call void @__tsan_write4(ptr %x)
entry:
  %x = alloca i32, align 4
  %i = ptrtoint ptr %x to i64
  store i64 %i, ptr @isink, align 8
  store i32 1, ptr %x, align 4
  ret void
}

define { ptr } @aggregate_returned() sanitize_thread {
; CHECK-LABEL: @aggregate_returned
; CHECK:       call void @__tsan_write4(ptr %x)
entry:
  %x = alloca i32, align 4
  store i32 1, ptr %x, align 4
  %agg = insertvalue { ptr } undef, ptr %x, 0
  ret { ptr } %agg
}

define void @freeze_is_transparent() sanitize_thread {
; CHECK-LABEL: @freeze_is_transparent
; CHECK-NOT:   call void @__tsan_write4
; CHECK:       ret void
entry:
  %x = alloca i32, align 4
  %f = freeze ptr %x
  store i32 1, ptr %f, align 4
  ret void
}
