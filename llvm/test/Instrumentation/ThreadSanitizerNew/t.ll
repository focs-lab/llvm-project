; RUN: opt < %s -passes=tsan -tsan-use-dominance-analysis -S | FileCheck %s

;---
; TEST 1: Basic pre-dominance (successful optimization)
; A store dominates a load to the same address. The path between them is clear.
; Expected: The load instrumentation should be removed.

define void @predom_write_reads_load(ptr %p) {
entry:
  ; CHECK-LABEL: @predom_write_reads_load
  ; The store should be instrumented.
  ; CHECK: call void @__tsan_write4(ptr %p)
  ; CHECK-NEXT: store i32 1, ptr %p, align 4
  store i32 1, ptr %p, align 4

  br label %middle

middle:
  ; The load should NOT be instrumented because it's dominated by the write.
  ; We check that the load itself is present, but no instrumentation call precedes it.
  ; CHECK: %val = load i32, ptr %p, align 4
  ; CHECK-NOT: call void @__tsan_read4
  %val = load i32, ptr %p, align 4

  ; CHECK: ret void
  ret void
}
