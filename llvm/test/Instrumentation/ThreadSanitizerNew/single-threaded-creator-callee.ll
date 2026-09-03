; A function called after pthread_create runs beside the new thread. It was
; classified single-threaded: the step that marks a thread creator's callees
; multi-threaded returned early because the creator had just been marked a
; creator. The control is called only from main before any thread exists.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-single-threaded -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g = global i32 0, align 4
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @body(ptr %a) sanitize_thread {
  store i32 2, ptr @g, align 4
  ret ptr null
}

define internal void @after_create() sanitize_thread {
; CHECK-LABEL: @after_create
; CHECK:       call void @__tsan_write4(ptr @g)
  store i32 1, ptr @g, align 4
  ret void
}

define internal void @before_any_thread() sanitize_thread {
; CHECK-LABEL: @before_any_thread
; CHECK-NOT:   call void @__tsan_write4(ptr @g)
; CHECK:       ret void
  store i32 0, ptr @g, align 4
  ret void
}

define internal void @spawn() sanitize_thread {
  %t = alloca i64, align 8
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @body, ptr null)
  call void @after_create()
  ret void
}

define i32 @main() sanitize_thread {
  call void @before_any_thread()
  call void @spawn()
  ret i32 0
}
