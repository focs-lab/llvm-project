; RUN: opt < %s -passes=tsan --tsan-use-dominance-analysis-postdom -S | FileCheck %s
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@GV1 = global i32 0, align 4
@GV2 = global i32 0, align 4

;---
; TEST 1: Basic post-dominance (successful optimization)
; A store in the merge block post-dominates writes in both branches.
; The paths from the branch writes to the merge write are clear.
; Expected: The write instrumentations in the branches should be removed.

define void @postdom_write_covers_write(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_write_covers_write
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; The write is post-dominated by the write in 'merge'.
  ; NO INSTRUMENTATION EXPECTED.
  ; CHECK-NOT:  call void @__tsan_write4
  ; CHECK:      store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4
  br label %merge

branch2:
  ; CHECK:      branch2:
  ; This write is also post-dominated.
  ; NO INSTRUMENTATION EXPECTED.
  ; CHECK-NOT:  call void @__tsan_write4
  ; CHECK:      store i32 2, ptr @GV1, align 4
  store i32 2, ptr @GV1, align 4
  br label %merge

merge:
  ; CHECK:      merge:
  ; This is the post-dominating store. It must be instrumented.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 3, ptr @GV1, align 4
  store i32 3, ptr @GV1, align 4
  ; CHECK:      ret void
  ret void
}

;---
; TEST 2: Negative case (path is not clear)
; The path from the write in branch1 to the merge block is "dirty" due to a function call.
; The path from branch2 is clear.
; Expected: The write in branch1 MUST be instrumented. The write in branch2 should be optimized away.

declare void @unknown_function()

define void @postdom_path_not_clear(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_path_not_clear
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; The path to the post-dominator is not clear, so this write must be instrumented.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4
  ; CHECK:      call void @unknown_function()
  call void @unknown_function()
  br label %merge

branch2:
  ; CHECK:      branch2:
  ; The path is clear, so this write should be optimized away.
  ; NO INSTRUMENTATION EXPECTED.
  ; CHECK-NOT:  call void @__tsan_write4
  ; CHECK:      store i32 2, ptr @GV1, align 4
  store i32 2, ptr @GV1, align 4
  br label %merge

merge:
  ; CHECK:      merge:
  ; The post-dominating store is always instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 3, ptr @GV1, align 4
  store i32 3, ptr @GV1, align 4
  ; CHECK:      ret void
  ret void
}

;---
; TEST 3: Negative case (incompatible access types)
; A load in the merge block post-dominates writes in the branches. A read cannot cover a write.
; Expected: All three accesses must be instrumented.

define void @postdom_read_does_not_cover_write(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_read_does_not_cover_write
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; This store must be instrumented because the post-dominating read does not cover it.
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
  ; The post-dominating load is instrumented as usual.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4
  ; CHECK:      ret void
  ret void
}

;---
; TEST 4: Negative case (different addresses)
; A store in the merge block does not alias the writes in the branches.
; Expected: All three accesses must be instrumented.

define void @postdom_different_addresses(i1 %cond) nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_different_addresses
  br i1 %cond, label %branch1, label %branch2

branch1:
  ; CHECK:      branch1:
  ; This store must be instrumented because the post-dominating store is to a different address.
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
  ; The post-dominating store to GV2 is instrumented as usual.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_write4(ptr @GV2)
  ; CHECK-NEXT: store i32 3, ptr @GV2, align 4
  store i32 3, ptr @GV2, align 4
  ; CHECK:      ret void
  ret void
}