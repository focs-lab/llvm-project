; A global is only safe to treat as read-only in multi-threaded code if nothing
; writes it there. Deciding that by comparing a store's pointer operand against
; the global itself recognised `g = x` for a scalar g and nothing else: `g[2] =
; x` goes through a constant getelementptr, and a write through any derived
; pointer was invisible, so an array written by every thread was still reported
; read-only -- and the consumer elides *all* accesses to such a global, not just
; the reads.

; RUN: opt < %s -passes='print<single-threaded>' -disable-output 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@ro = global i32 0, align 4                       ; written before threads, read after
@arr = global [4 x i32] zeroinitializer, align 16 ; written by a thread via a constant GEP
@escaped = global i32 0, align 4                  ; address handed to a thread

declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @worker(ptr %arg) nounwind {
entry:
  %v = load i32, ptr @ro, align 4
  ; constant getelementptr: not an Instruction user of @arr at all
  store i32 %v, ptr getelementptr inbounds ([4 x i32], ptr @arr, i64 0, i64 2), align 4
  ; and through the pointer it was handed
  store i32 1, ptr %arg, align 4
  ret ptr null
}

define i32 @main() nounwind {
entry:
  %t = alloca i64, align 8
  store i32 5, ptr @ro, align 4
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr @escaped)
  ret i32 0
}

; CHECK: Read-Only Globals
; CHECK: ro
; CHECK-NOT: arr
; CHECK-NOT: escaped
