; TSan's file-descriptor interceptors are synchronisation: write(2) releases
; and read(2) acquires on the descriptor, so a race with the other end of a
; pipe is ordered through them. A dominating access must not be treated as
; covering one on the far side of write, nor a post-dominating one across
; read. The library table answered "sync-free" for any function it had no
; entry for, and both were elided. strlen is the control: it really is
; sync-free, so the redundant store across it is elided.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4
declare i64 @write(i32, ptr, i64)
declare i64 @read(i32, ptr, i64)
declare i64 @strlen(ptr)

; Both stores kept: dominance may not cross write(2).
; CHECK-LABEL: @across_write
; CHECK-COUNT-2: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @across_write(ptr %buf) sanitize_thread {
  store i32 1, ptr @x, align 4
  %n = call i64 @write(i32 1, ptr %buf, i64 1)
  store i32 2, ptr @x, align 4
  ret void
}

; Both stores kept: post-dominance may not cross read(2).
; CHECK-LABEL: @across_read
; CHECK-COUNT-2: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @across_read(ptr %buf) sanitize_thread {
  store i32 1, ptr @x, align 4
  %n = call i64 @read(i32 0, ptr %buf, i64 1)
  store i32 2, ptr @x, align 4
  ret void
}

; Control: strlen is sync-free, so the second (dominated) store is elided.
; CHECK-LABEL: @across_strlen
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK-NOT: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @across_strlen(ptr %buf) sanitize_thread {
  store i32 1, ptr @x, align 4
  %n = call i64 @strlen(ptr %buf)
  store i32 2, ptr @x, align 4
  ret void
}
