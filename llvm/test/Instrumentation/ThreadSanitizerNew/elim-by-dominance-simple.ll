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


;---
; TEST 2: Negative case (path is not clear)
; A store dominates a load, but an unknown function call is between them.
; Expected: Optimization should be disabled; both accesses must be instrumented.

declare void @unknown_function()

define void @predom_path_not_clear(ptr %p) {
entry:
  ; CHECK-LABEL: @predom_path_not_clear
  ; First, the store is instrumented.
  ; CHECK: call void @__tsan_write4(ptr %p)
  ; CHECK-NEXT: store i32 1, ptr %p, align 4
  store i32 1, ptr %p, align 4

  ; The "dangerous" call remains.
  ; CHECK: call void @unknown_function()
  call void @unknown_function()

  ; The load is also instrumented because the path was not clear.
  ; CHECK: call void @__tsan_read4(ptr %p)
  ; CHECK-NEXT: %val = load i32, ptr %p, align 4
  %val = load i32, ptr %p, align 4

  ; CHECK: ret void
  ret void
}


;---
; TEST 3: Negative case (incompatible access types)
; A load dominates a store. A read cannot cover a write.
; Expected: Optimization should not apply; both accesses must be instrumented.

define void @predom_read_does_not_cover_write(ptr %p) {
entry:
  ; CHECK-LABEL: @predom_read_does_not_cover_write
  ; The load is instrumented.
  ; CHECK: call void @__tsan_read4(ptr %p)
  ; CHECK-NEXT: %val = load i32, ptr %p, align 4
  %val = load i32, ptr %p, align 4

  ; The store is also instrumented.
  ; CHECK: call void @__tsan_write4(ptr %p)
  ; CHECK-NEXT: store i32 1, ptr %p, align 4
  store i32 1, ptr %p, align 4

  ; CHECK: ret void
  ret void
}


;---
; TEST 4: Negative case (different addresses)
; A store to one address dominates a load from another.
; Expected: Optimization should not apply as addresses do not alias.

define void @predom_different_addresses() {
entry:
  ; CHECK-LABEL: @predom_different_addresses
  ; CHECK: %p1 = alloca i32
  %p1 = alloca i32
  ; CHECK: %p2 = alloca i32
  %p2 = alloca i32

  ; The store to p1 is instrumented.
  ; CHECK: call void @__tsan_write4(ptr %p1)
  ; CHECK-NEXT: store i32 1, ptr %p1, align 4
  store i32 1, ptr %p1, align 4

  ; The load from p2 is also instrumented.
  ; CHECK: call void @__tsan_read4(ptr %p2)
  ; CHECK-NEXT: %val = load i32, ptr %p2, align 4
  %val = load i32, ptr %p2, align 4

  ; CHECK: ret void
  ret void
}