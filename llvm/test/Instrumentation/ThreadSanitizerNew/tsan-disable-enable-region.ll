; __tsan_disable / __tsan_enable bracket a region whose accesses are not
; instrumented; the pass erases the calls themselves (the runtime symbols
; are no-ops kept for code that names them directly). Accesses outside the
; region are instrumented as usual; nesting counts.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4
declare void @__tsan_disable()
declare void @__tsan_enable()

; CHECK-LABEL: @region
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK-NOT: call void @__tsan_disable
; CHECK-NOT: call void @__tsan_write4
; CHECK-NOT: call void @__tsan_enable
; CHECK: store i32 3, ptr @x
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @region() sanitize_thread {
  store i32 1, ptr @x, align 4
  call void @__tsan_disable()
  store i32 2, ptr @x, align 4
  call void @__tsan_disable()
  store i32 3, ptr @x, align 4
  call void @__tsan_enable()
  store i32 3, ptr @x, align 4
  call void @__tsan_enable()
  store i32 4, ptr @x, align 4
  ret void
}
