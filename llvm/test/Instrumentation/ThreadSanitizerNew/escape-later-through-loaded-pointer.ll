; The sound flow-sensitive rule asks what escapes the object AFTER the access.
; It must ask about the object the escape analysis reasoned about, not the SSA
; value the address happens to be: here the address is a pointer loaded from a
; slot, the object behind it is x, and x is published by a plain store in the
; next block. The walk from the load saw no escape and the write was elided.
;
; Through a loaded pointer the rule cannot enumerate x's later uses at all, so
; it keeps the access whatever the later publication is -- a plain store OR a
; thread creation. Both are kept here; the case where a thread-creation
; publication of a DIRECTLY accessed local is elided is in
; escape-adversarial.ll (@published_via_thread_create).

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@sink = global ptr null, align 8
declare i32 @pthread_create(ptr, ptr, ptr, ptr)
define internal ptr @worker(ptr %a) { ret ptr null }

define void @plain_store_later() sanitize_thread {
; CHECK-LABEL: @plain_store_later
; CHECK:       call void @__tsan_write4(ptr %p)
entry:
  %s = alloca ptr, align 8
  %x = alloca i32, align 4
  store ptr %x, ptr %s, align 8
  %p = load ptr, ptr %s, align 8
  store i32 1, ptr %p, align 4
  br label %later
later:
  store ptr %x, ptr @sink, align 8
  ret void
}

define void @thread_create_later() sanitize_thread {
; CHECK-LABEL: @thread_create_later
; CHECK:       call void @__tsan_write4(ptr %p)
entry:
  %s = alloca ptr, align 8
  %x = alloca i32, align 4
  %t = alloca i64, align 8
  store ptr %x, ptr %s, align 8
  %p = load ptr, ptr %s, align 8
  store i32 1, ptr %p, align 4
  br label %later
later:
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr %x)
  ret void
}
