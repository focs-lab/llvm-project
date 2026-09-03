; A callee reached through an invoke counts like one reached through a call:
; @g invokes an opaque function, so @g synchronizes, and a call to @g between
; two stores keeps both. The summary's scans only looked at CallInst; the
; per-instruction classification still caught the invoke, so this is a
; guard for the CallBase form of the scans, not a reproducer.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4
declare void @opaque()
declare i32 @__gxx_personality_v0(...)

define internal void @g() personality ptr @__gxx_personality_v0 {
  invoke void @opaque() to label %ok unwind label %lp
ok:
  ret void
lp:
  %l = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %l
}

; CHECK-LABEL: @f
; CHECK-COUNT-2: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @f() sanitize_thread {
  store i32 1, ptr @x, align 4
  call void @g()
  store i32 2, ptr @x, align 4
  ret void
}
