; Top-down argument escape. @direct is a local function called only
; directly with a pointer to a local that never escapes, so its argument is
; known not to escape and the access through it is elided. @tabled is
; identical but its address sits in a dispatch table's initializer: it has
; no direct call site at all, and the top-down pass used to record "no
; caller passes an escaped pointer" as "the argument does not escape".
; Anyone holding the table can call it with anything, so its access stays.
; @registered is handed to a defined function that only stores it; same
; verdict.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@table = global [1 x ptr] [ptr @tabled], align 8
@slot = global ptr null, align 8

; CHECK-LABEL: define internal void @direct
; CHECK-NOT: call void @__tsan_write4
; CHECK: ret void
define internal void @direct(ptr %p) sanitize_thread {
  store i32 1, ptr %p, align 4
  ret void
}

; CHECK-LABEL: define internal void @tabled
; CHECK: call void @__tsan_write4(ptr %p)
; CHECK: ret void
define internal void @tabled(ptr %p) sanitize_thread {
  store i32 2, ptr %p, align 4
  ret void
}

; CHECK-LABEL: define internal void @registered
; CHECK: call void @__tsan_write4(ptr %p)
; CHECK: ret void
define internal void @registered(ptr %p) sanitize_thread {
  store i32 3, ptr %p, align 4
  ret void
}

define internal void @keep(ptr %f) {
  store ptr %f, ptr @slot, align 8
  ret void
}

define void @main() sanitize_thread {
  %l = alloca i32, align 4
  call void @direct(ptr %l)
  call void @keep(ptr @registered)
  ret void
}
