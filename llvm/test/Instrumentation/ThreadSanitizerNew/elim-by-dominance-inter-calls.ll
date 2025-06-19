; RUN: opt < %s -passes='module(tsan-module),function(tsan)' --tsan-use-dominance-analysis -S | FileCheck %s
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@GV1 = global i32 0, align 4

; --- Helper functions for testing SyncFreeInfo ---

; A provably "clean" function with no side effects.
define internal void @clean_function() nounwind {
  ret void
}

; A provably "dangerous" function containing a fence.
define internal void @dangerous_func() nounwind {
  fence seq_cst
  ret void
}

; A wrapper that becomes dangerous by calling a dangerous function.
define internal void @wrapper_func() nounwind {
  call void @dangerous_func()
  ret void
}

;---
; TEST 6: Call to a provably "clean" function
; The path contains a call to a function that SyncFreeInfo can prove is safe.
; Expected: The optimization should trigger, as the path is considered clear.

define void @predom_with_clean_call() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_with_clean_call
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4

  ; CHECK:      call void @clean_function()
  call void @clean_function()

  ; The load should be optimized away because @clean_function is sync-free.
  ; NO INSTRUMENTATION EXPECTED.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4

  ; CHECK:      ret void
  ret void
}

;---
; TEST 7: Call to a transitively "dangerous" function
; The path contains a call to a function that is not directly dangerous, but calls one that is.
; Expected: SyncFreeInfo should propagate the "dangerous" status, disabling the optimization.

define void @predom_with_transitive_danger() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_with_transitive_danger
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4

  ; CHECK:      call void @wrapper_func()
  call void @wrapper_func()

  ; The load should NOT be optimized away, as @wrapper_func is transitively dangerous.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4

  ; CHECK:      ret void
  ret void
}

; --- Helper functions for SCC tests ---

define internal void @recursive_a_clean(i32 %n) nounwind {
  %cmp = icmp sgt i32 %n, 0
  br i1 %cmp, label %recurse, label %exit
recurse:
  %next = sub i32 %n, 1
  call void @recursive_b_clean(i32 %next)
  br label %exit
exit:
  ret void
}

define internal void @recursive_b_clean(i32 %n) nounwind {
  %cmp = icmp sgt i32 %n, 0
  br i1 %cmp, label %recurse, label %exit
recurse:
  %next = sub i32 %n, 1
  call void @recursive_a_clean(i32 %next)
  br label %exit
exit:
  ret void
}

;---
; TEST 8: Call to a "clean" SCC (Strongly Connected Component)
; The path contains a call to mutually recursive functions that are all clean.
; Expected: SyncFreeInfo should correctly identify the entire SCC as safe, enabling the optimization.

define void @predom_with_clean_scc() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_with_clean_scc
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4

  ; CHECK:      call void @recursive_a_clean(i32 2)
  call void @recursive_a_clean(i32 2)

  ; The load should be optimized away because the entire recursive call chain is sync-free.
  ; NO INSTRUMENTATION EXPECTED.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4

  ; CHECK:      ret void
  ret void
}

; --- Helper functions for dangerous SCC test ---

define internal void @recursive_a_dangerous(i32 %n) nounwind {
  %cmp = icmp sgt i32 %n, 0
  br i1 %cmp, label %recurse, label %exit
recurse:
  %next = sub i32 %n, 1
  call void @recursive_b_dangerous(i32 %next)
  br label %exit
exit:
  ret void
}

define internal void @recursive_b_dangerous(i32 %n) nounwind {
  call void @dangerous_func() ; This makes the whole SCC dangerous
  %cmp = icmp sgt i32 %n, 0
  br i1 %cmp, label %recurse, label %exit
recurse:
  %next = sub i32 %n, 1
  call void @recursive_a_dangerous(i32 %next)
  br label %exit
exit:
  ret void
}

;---
; TEST 9: Call to a "dangerous" SCC
; The path contains a call to mutually recursive functions, one of which is dangerous.
; Expected: SyncFreeInfo should propagate the "dangerous" status across the entire SCC, disabling the optimization.

define void @predom_with_dangerous_scc() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_with_dangerous_scc
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4

  ; CHECK:      call void @recursive_a_dangerous(i32 2)
  call void @recursive_a_dangerous(i32 2)

  ; The load should NOT be optimized away, as the call goes into a dangerous SCC.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4

  ; CHECK:      ret void
  ret void
}