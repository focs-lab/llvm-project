; RUN: opt < %s -passes=tsan --tsan-use-dominance-analysis-dom -S | FileCheck %s
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g_var = global i32 0, align 4
@g_var2 = global i32 0, align 4

;---
; TEST 1: Basic pre-dominance (successful optimization)
; A store dominates a load to the same global variable. The path between them is clear.
; Expected: The load instrumentation should be removed by the dominance analysis.

define void @predom_write_reads_load() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_write_reads_load
  ; The store to the global variable must be instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @g_var)
  ; CHECK-NEXT: store i32 1, ptr @g_var, align 4
  store i32 1, ptr @g_var, align 4

  br label %middle

middle:
  ; The load is dominated by the write and the path is clear.
  ; NO INSTRUMENTATION EXPECTED for the load.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val = load i32, ptr @g_var, align 4
  %val = load i32, ptr @g_var, align 4

  ; CHECK:      ret void
  ret void
}


;---
; TEST 2: Negative case (path is not clear)
; A store dominates a load, but an unknown function call is between them, making the path "dirty".
; Expected: Optimization should be disabled; both accesses must be instrumented.

declare void @unknown_function()

define void @predom_path_not_clear() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_path_not_clear
  ; The store must be instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @g_var)
  ; CHECK-NEXT: store i32 1, ptr @g_var, align 4
  store i32 1, ptr @g_var, align 4

  ; The "dangerous" call remains.
  call void @unknown_function()

  ; The load must also be instrumented because the path was not clear.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @g_var)
  ; CHECK-NEXT: %val = load i32, ptr @g_var, align 4
  %val = load i32, ptr @g_var, align 4

  ; CHECK:      ret void
  ret void
}


;---
; TEST 3: Negative case (incompatible access types)
; A load dominates a store. A read cannot cover a write for race detection purposes.
; Expected: Optimization should not apply; both accesses must be instrumented.

define void @predom_read_does_not_cover_write() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_read_does_not_cover_write
  ; The load must be instrumented.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @g_var)
  ; CHECK-NEXT: %val = load i32, ptr @g_var, align 4
  %val = load i32, ptr @g_var, align 4

  ; The store must also be instrumented.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_write4(ptr @g_var)
  ; CHECK-NEXT: store i32 1, ptr @g_var, align 4
  store i32 1, ptr @g_var, align 4

  ; CHECK:      ret void
  ret void
}


;---
; TEST 4: Negative case (different addresses)
; A store to one global variable dominates a load from another.
; Expected: Optimization should not apply as addresses do not alias.

define void @predom_different_addresses() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_different_addresses
  ; The store to g_var must be instrumented.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_write4(ptr @g_var)
  ; CHECK-NEXT: store i32 1, ptr @g_var, align 4
  store i32 1, ptr @g_var, align 4

  ; The load from g_var2 must also be instrumented as it's a different object.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @g_var2)
  ; CHECK-NEXT: %val = load i32, ptr @g_var2, align 4
  %val = load i32, ptr @g_var2, align 4

  ; CHECK:      ret void
  ret void
}