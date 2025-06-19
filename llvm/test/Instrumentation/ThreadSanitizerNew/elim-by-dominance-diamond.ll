; RUN: opt < %s -passes=tsan --tsan-use-dominance-analysis-dom -S | FileCheck %s
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@GV1 = global i32 0, align 4
@GV2 = global i32 0, align 4

;---
; TEST 1: Basic pre-dominance (successful optimization)
; A store in the entry block pre-dominates loads in both branches.
; The paths to the loads are clear.
; Expected: The load instrumentations in both branches should be removed.

define void @predom_diamond_write_covers_load(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_diamond_write_covers_load
  ; This is the pre-dominating store. It must be instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; This load is dominated by the store in 'entry'.
  ; NO INSTRUMENTATION EXPECTED.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val1 = load i32, ptr @GV1, align 4
  %val1 = load i32, ptr @GV1, align 4
  br label %merge

branch2:
  ; CHECK:      branch2:
  ; This load is also dominated.
  ; NO INSTRUMENTATION EXPECTED.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val2 = load i32, ptr @GV1, align 4
  %val2 = load i32, ptr @GV1, align 4
  br label %merge

merge:
  ; CHECK:      merge:
  ; CHECK:      ret void
  ret void
}

;---
; TEST 2: Negative case (path is not clear)
; The path to the load in branch1 is "dirty" due to a function call. The path to branch2 is clear.
; Expected: The load in branch1 must be instrumented. The load in branch2 should be optimized away.

declare void @unknown_function()

define void @predom_diamond_path_not_clear(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_diamond_path_not_clear
  ; The pre-dominating store is instrumented as usual.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; The path from the dominator is not clear, so this load must be instrumented.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @unknown_function()
  call void @unknown_function()
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val1 = load i32, ptr @GV1, align 4
  %val1 = load i32, ptr @GV1, align 4
  br label %merge

branch2:
  ; CHECK:      branch2:
  ; The path is clear, so this load should be optimized away.
  ; NO INSTRUMENTATION EXPECTED.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val2 = load i32, ptr @GV1, align 4
  %val2 = load i32, ptr @GV1, align 4
  br label %merge

merge:
  ; CHECK:      merge:
  ; CHECK:      ret void
  ret void
}

;---
; TEST 3: Negative case (incompatible access types)
; A load in the entry block pre-dominates stores in the branches. A read cannot cover a write.
; Expected: All three accesses must be instrumented.

define void @predom_diamond_read_does_not_cover_write(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_diamond_read_does_not_cover_write
  ; The pre-dominating load is instrumented.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; This store must be instrumented because the pre-dominating read does not cover it.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4
  br label %merge

branch2:
  ; CHECK:      branch2:
  ; This store must also be instrumented.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 2, ptr @GV1, align 4
  store i32 2, ptr @GV1, align 4
  br label %merge

merge:
  ; CHECK:      merge:
  ; CHECK:      ret void
  ret void
}

;---
; TEST 4: Negative case (different addresses)
; A store to GV1 in the entry block pre-dominates loads from GV2 in the branches.
; Expected: All three accesses must be instrumented.

define void @predom_diamond_different_addresses(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @predom_diamond_different_addresses
  ; The pre-dominating store to GV1 is instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; This load from GV2 must be instrumented as it's a different object.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @GV2)
  ; CHECK-NEXT: %val1 = load i32, ptr @GV2, align 4
  %val1 = load i32, ptr @GV2, align 4
  br label %merge

branch2:
  ; CHECK:      branch2:
  ; This load from GV2 must also be instrumented.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @GV2)
  ; CHECK-NEXT: %val2 = load i32, ptr @GV2, align 4
  %val2 = load i32, ptr @GV2, align 4
  br label %merge

merge:
  ; CHECK:      merge:
  ; CHECK:      ret void
  ret void
}