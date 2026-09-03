; The library retention table, one entry per section: atexit stores its
; function-pointer argument for later (section I: escapes), strlen and free
; do not retain theirs (section II), and a function TargetLibraryInfo does
; not know (a program's own name with a library-like spelling and the
; wrong prototype) is opaque.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

declare i64 @strlen(ptr)
declare void @free(ptr)
declare ptr @malloc(i64)
declare i32 @atexit(ptr)
declare i64 @strnlen(ptr, i64, i64)

; CHECK-LABEL: @via_strlen
; CHECK-NOT: call void @__tsan_write1
; CHECK: ret i64
define i64 @via_strlen() sanitize_thread {
  %b = alloca [8 x i8], align 1
  store i8 0, ptr %b, align 1
  %n = call i64 @strlen(ptr %b)
  ret i64 %n
}

; CHECK-LABEL: @via_free
; CHECK-NOT: call void @__tsan_write4
; CHECK: {{call|invoke}} void @free
define void @via_free() sanitize_thread {
  %p = call ptr @malloc(i64 4)
  store i32 1, ptr %p, align 4
  call void @free(ptr %p)
  ret void
}

; CHECK-LABEL: @wrong_prototype
; CHECK: call void @__tsan_write1
define i64 @wrong_prototype() sanitize_thread {
  %b = alloca [8 x i8], align 1
  store i8 0, ptr %b, align 1
  %n = call i64 @strnlen(ptr %b, i64 8, i64 0)
  ret i64 %n
}
