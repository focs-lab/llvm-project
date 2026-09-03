; Intrinsics on the dominance path. Most intrinsics are sync-free and a
; redundant access across them is elided; the blocklist names the ones that
; synchronise -- a GPU barrier among them -- and those block elimination.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4
declare void @llvm.assume(i1)
declare void @llvm.nvvm.barrier0()

; CHECK-LABEL: @across_assume
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK-NOT: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @across_assume(i1 %c) sanitize_thread {
  store i32 1, ptr @x, align 4
  call void @llvm.assume(i1 %c)
  store i32 2, ptr @x, align 4
  ret void
}

; CHECK-LABEL: @across_barrier
; CHECK-COUNT-2: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @across_barrier() sanitize_thread {
  store i32 1, ptr @x, align 4
  call void @llvm.nvvm.barrier0()
  store i32 2, ptr @x, align 4
  ret void
}
