; A global another translation unit can name may be written there; only a
; global with local linkage is fully visible here. An external global never
; written in this unit was classified read-only and every access to it elided
; (memcached's current_time, written by the main thread's clock callback in
; another file and read unsynchronised by workers). The control is internal,
; written only before threads exist, read by a thread: read-only, elided.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-swmr -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@ext = external global i32, align 4
@loc = internal global i32 0, align 4
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @body(ptr %a) sanitize_thread {
; CHECK-LABEL: @body
; CHECK:       call void @__tsan_read4(ptr @ext)
; CHECK-NOT:   call void @__tsan_read4(ptr @loc)
; CHECK:       ret ptr null
  %e = load i32, ptr @ext, align 4
  %l = load i32, ptr @loc, align 4
  ret ptr null
}

define i32 @main() sanitize_thread {
  store i32 1, ptr @loc, align 4
  %t = alloca i64, align 8
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @body, ptr null)
  ret i32 0
}
