; Library functions that keep a pointer. setvbuf installs the caller's
; buffer as the stream's buffer: every later read or write on the stream,
; from any thread, goes through it, so a write to the buffer after setvbuf
; races with them and must stay instrumented. It was tabulated as
; non-retaining. realloc may return the block it was given, so the result
; aliases whatever aliased the argument; it was tabulated as fresh memory,
; and an access through the result of reallocating a published block looked
; local. @local_only is the control: a buffer handed only to strlen.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct._IO_FILE = type opaque
@stream = external global ptr, align 8
@published = global ptr null, align 8
declare i32 @setvbuf(ptr, ptr, i32, i64)
declare i64 @strlen(ptr)
declare ptr @realloc(ptr, i64)

; CHECK-LABEL: define void @stream_buffer
; CHECK: {{call|invoke}} i32 @setvbuf
; CHECK: call void @__tsan_write1(ptr %buf)
define void @stream_buffer() sanitize_thread {
  %buf = alloca [64 x i8], align 1
  %s = load ptr, ptr @stream, align 8
  %r = call i32 @setvbuf(ptr %s, ptr %buf, i32 0, i64 64)
  store i8 65, ptr %buf, align 1
  ret void
}

; CHECK-LABEL: define void @reallocated
; CHECK: {{call|invoke}} ptr @realloc
; CHECK: call void @__tsan_write4(ptr %n{{[0-9]*}})
define void @reallocated() sanitize_thread {
  %p = load ptr, ptr @published, align 8
  %n = call ptr @realloc(ptr %p, i64 64)
  store i32 1, ptr %n, align 4
  ret void
}

; CHECK-LABEL: define i64 @local_only
; CHECK-NOT: call void @__tsan_write1
; CHECK: ret i64
define i64 @local_only() sanitize_thread {
  %buf = alloca [64 x i8], align 1
  store i8 0, ptr %buf, align 1
  %n = call i64 @strlen(ptr %buf)
  ret i64 %n
}
