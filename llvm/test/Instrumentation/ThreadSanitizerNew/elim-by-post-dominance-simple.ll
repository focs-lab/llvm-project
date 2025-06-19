; RUN: opt < %s -passes=tsan --tsan-use-dominance-analysis-postdom -S | FileCheck %s
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@GV1 = global i32 0, align 4
@GV2 = global i32 0, align 4

;---
; TEST 1: Basic post-dominance (successful optimization)
; A write post-dominates a read to the same global variable within one basic block.
; Expected: The read instrumentation should be removed by the post-dominance analysis.

define void @postdom_simple_write_covers_read() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_simple_write_covers_read
  ; This read is post-dominated by the write below and should be optimized away.
  ; NO INSTRUMENTATION EXPECTED.
  ; CHECK-NOT:  call void @__tsan_read4
  ; CHECK:      %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4

  ; This write is the post-dominator and must be instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4

  ; CHECK:      ret void
  ret void
}

;---
; TEST 2: Negative case (path is not clear)
; Path between the read and the post-dominating write is "dirty" due to a function call.
; Expected: Optimization should be disabled; both accesses must be instrumented.

declare void @unknown_function()

define void @postdom_simple_path_not_clear() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_simple_path_not_clear
  ; This read must be instrumented because the path to the post-dominator is not clear.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4

  ; The "dangerous" call.
  ; CHECK:      call void @unknown_function()
  call void @unknown_function()

  ; The post-dominating write is instrumented as usual.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4

  ; CHECK:      ret void
  ret void
}

;---
; TEST 3: Negative case (incompatible access types)
; A read post-dominates a write. A read cannot cover a write.
; Expected: Both accesses must be instrumented.

define void @postdom_simple_read_does_not_cover_write() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_simple_read_does_not_cover_write
  ; This write must be instrumented because the post-dominating read does not cover it.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_write4(ptr @GV1)
  ; CHECK-NEXT: store i32 1, ptr @GV1, align 4
  store i32 1, ptr @GV1, align 4

  ; The post-dominating read must also be instrumented.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4

  ; CHECK:      ret void
  ret void
}

;---
; TEST 4: Negative case (different addresses)
; A write to GV2 post-dominates a read from GV1.
; Expected: Optimization should not apply as addresses do not alias.

define void @postdom_simple_different_addresses() nounwind uwtable sanitize_thread {
entry:
  ; CHECK-LABEL: @postdom_simple_different_addresses
  ; This read must be instrumented.
  ; INSTRUMENTATION EXPECTED.
  ; CHECK:      call void @__tsan_read4(ptr @GV1)
  ; CHECK-NEXT: %val = load i32, ptr @GV1, align 4
  %val = load i32, ptr @GV1, align 4

  ; The post-dominating write to a different address must also be instrumented.
  ; CHECK:      call void @__tsan_write4(ptr @GV2)
  ; CHECK-NEXT: store i32 1, ptr @GV2, align 4
  store i32 1, ptr @GV2, align 4

  ; CHECK:      ret void
  ret void
}