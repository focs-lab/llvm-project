; Volatile accesses under dominance elimination. With
; -tsan-distinguish-volatile a volatile access is instrumented with its own
; runtime entry point, so a volatile access and a plain one to the same
; location cannot stand in for each other: neither covers the other.
; Without the flag they are ordinary accesses and the second store is
; redundant.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis -tsan-distinguish-volatile -S | FileCheck %s
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis -S | FileCheck %s --check-prefix=PLAIN

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4

; CHECK-LABEL: @mixed
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK: call void @__tsan_volatile_write4(ptr @x)
; CHECK: ret void
; PLAIN-LABEL: @mixed
; PLAIN: call void @__tsan_write4(ptr @x)
; PLAIN-NOT: call void @__tsan_write4(ptr @x)
; PLAIN: ret void
define void @mixed() sanitize_thread {
  store i32 1, ptr @x, align 4
  store volatile i32 2, ptr @x, align 4
  ret void
}
