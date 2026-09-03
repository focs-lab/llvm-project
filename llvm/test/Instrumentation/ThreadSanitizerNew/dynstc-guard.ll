; Dynamic single-threaded check. With -tsan-use-active-thread-count every
; run of accesses (ended by a call or a block boundary) is guarded by one
; load of the runtime's active-thread counter: the leader is instrumented
; under the guard and the rest of the run shares it. The counter load is a
; monotonic atomic on a runtime-owned global and is never itself
; instrumented; the calls are still emitted, only conditionally.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-active-thread-count -S | FileCheck %s
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -S | FileCheck %s --check-prefix=PLAIN

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4
@y = global i32 0, align 4
declare void @opaque()

; CHECK-LABEL: @two_runs
; CHECK: load atomic i32, ptr @__tsan_active_thread_count monotonic
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK: call void @__tsan_write4(ptr @y)
; CHECK: {{call|invoke}} void @opaque()
; CHECK: load atomic i32, ptr @__tsan_active_thread_count monotonic
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK: ret void
; PLAIN-LABEL: @two_runs
; PLAIN-NOT: __tsan_active_thread_count
; PLAIN: ret void
define void @two_runs() sanitize_thread {
  store i32 1, ptr @x, align 4
  store i32 1, ptr @y, align 4
  call void @opaque()
  store i32 2, ptr @x, align 4
  ret void
}
