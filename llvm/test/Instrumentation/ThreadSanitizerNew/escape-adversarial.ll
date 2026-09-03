; Ways an object can escape that a points-to analysis might miss, and the
; boundaries of what this one deliberately does not treat as escaping. Every
; assertion here was confirmed against the built pass, not assumed.
;
; The property under test is the one that matters downstream: if the object is
; reachable from another thread, the write to it must stay instrumented. Each
; function holds one object of interest.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s --check-prefixes=CHECK,SOUND
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -tsan-ea-sound-flow-sensitive=false -S | FileCheck %s --check-prefixes=CHECK,BARE

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@sink = global ptr null, align 8
@sink_struct = global { ptr } zeroinitializer, align 8
@sink_array = global [2 x ptr] zeroinitializer, align 16

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1)
declare i64 @strlen(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

; ---------------------------------------------------------------------------
; Escapes that must be caught: a pointer into a local copied into global memory.
; ---------------------------------------------------------------------------

;--- via memcpy of a struct that contains the pointer.
define void @memcpy_struct_with_pointer() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @memcpy_struct_with_pointer
; CHECK:       call void @__tsan_write4(ptr %x)
entry:
  %x = alloca i32, align 4
  %s = alloca { ptr }, align 8
  store ptr %x, ptr %s, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr @sink_struct, ptr %s, i64 8, i1 false)
  store i32 1, ptr %x, align 4
  ret void
}

;--- via memmove rather than memcpy. Only memcpy was modelled before; memmove
;--- copies the pointer just the same.
define void @memmove_struct_with_pointer() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @memmove_struct_with_pointer
; CHECK:       call void @__tsan_write4(ptr %x)
entry:
  %x = alloca i32, align 4
  %s = alloca { ptr }, align 8
  store ptr %x, ptr %s, align 8
  call void @llvm.memmove.p0.p0.i64(ptr @sink_struct, ptr %s, i64 8, i1 false)
  store i32 1, ptr %x, align 4
  ret void
}

;--- via an array of pointers rather than a struct. structContainsPointerType
;--- returned false for arrays, so this was missed too.
define void @memcpy_pointer_array() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @memcpy_pointer_array
; CHECK:       call void @__tsan_write4(ptr %x)
entry:
  %x = alloca i32, align 4
  %a = alloca [2 x ptr], align 16
  store ptr %x, ptr %a, align 16
  call void @llvm.memcpy.p0.p0.i64(ptr @sink_array, ptr %a, i64 16, i1 false)
  store i32 1, ptr %x, align 4
  ret void
}

;--- via a store of a pointer-to-a-pointer chain. x is stored into s, s is
;--- published; x escapes transitively.
define void @transitive_store() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @transitive_store
; CHECK:       call void @__tsan_write4(ptr %x)
entry:
  %x = alloca i32, align 4
  %s = alloca ptr, align 8
  store ptr %x, ptr %s, align 8
  store ptr %s, ptr @sink, align 8
  store i32 1, ptr %x, align 4
  ret void
}

; ---------------------------------------------------------------------------
; Merges: if a pointer may be an escaped object, it must be kept.
; ---------------------------------------------------------------------------

define void @phi_merges_escaped(i1 %c) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @phi_merges_escaped
; CHECK:       call void @__tsan_write4(ptr %p)
entry:
  %x = alloca i32, align 4
  br i1 %c, label %a, label %b
a:
  br label %join
b:
  %g = load ptr, ptr @sink, align 8
  br label %join
join:
  %p = phi ptr [ %x, %a ], [ %g, %b ]
  store i32 1, ptr %p, align 4
  ret void
}

define void @select_merges_escaped(i1 %c) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @select_merges_escaped
; CHECK:       call void @__tsan_write4(ptr %p)
entry:
  %x = alloca i32, align 4
  %g = load ptr, ptr @sink, align 8
  %p = select i1 %c, ptr %x, ptr %g
  store i32 1, ptr %p, align 4
  ret void
}

;--- a pointer rotated through a loop-carried phi: after the first iteration it
;--- may be anything loaded from escaped memory.
define void @loop_rotated_pointer(i32 %n) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @loop_rotated_pointer
; CHECK:       call void @__tsan_write4(ptr %p)
entry:
  %x = alloca i32, align 4
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %p = phi ptr [ %x, %entry ], [ %q, %loop ]
  store i32 1, ptr %p, align 4
  %q = load ptr, ptr @sink, align 8
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; ---------------------------------------------------------------------------
; Boundaries the analysis draws on purpose. These are sound for programs
; without out-of-bounds pointer arithmetic, and pin the design so that
; changing it is a deliberate act.
; ---------------------------------------------------------------------------

;--- Field-sensitive: publishing a pointer to field 1 escapes field 1, not the
;--- unrelated field 0. A remote thread can only reach field 0 by computing
;--- out of bounds from &s.f1, which is undefined. So the write to field 0 is
;--- elided; the publication store to the global is kept.
define void @field_sensitive_other_field() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @field_sensitive_other_field
; CHECK:       call void @__tsan_write8(ptr @sink)
; CHECK-NOT:   call void @__tsan_write4(ptr %s)
; CHECK:       ret void
entry:
  %s = alloca { i32, i32 }, align 8
  %f1 = getelementptr inbounds { i32, i32 }, ptr %s, i64 0, i32 1
  store ptr %f1, ptr @sink, align 8
  store i32 1, ptr %s, align 8
  ret void
}

;--- Escape analysis still eliding at all: strlen retains nothing, which only
;--- escape analysis knows -- capture tracking sees a call and would keep it.
define void @strlen_elided() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @strlen_elided
; CHECK-NOT:   call void @__tsan_write1(ptr %buf)
; CHECK:       ret void
entry:
  %buf = alloca [8 x i8], align 1
  %n = call i64 @strlen(ptr %buf)
  store i8 0, ptr %buf, align 1
  ret void
}

;--- A write before the object is published by a plain store in a later
;--- block. Per-point escape analysis alone would elide it, and with it the
;--- report stock TSan gives on x (the publication is still reported). The
;--- sound rule keeps it: the later escape is a plain store, which another
;--- thread can receive unordered. The bare per-point elision is kept behind
;--- -tsan-ea-sound-flow-sensitive=false for measurement.
define void @published_in_later_block() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @published_in_later_block
; SOUND:       call void @__tsan_write4(ptr %x)
; BARE-NOT:    call void @__tsan_write4(ptr %x)
; CHECK:       call void @__tsan_write8(ptr @sink)
; CHECK:       ret void
entry:
  %x = alloca i32, align 4
  store i32 1, ptr %x, align 4
  br label %later
later:
  store ptr %x, ptr @sink, align 8
  ret void
}

;--- The same shape, but the later escape is a thread creation. The child
;--- starts after the write, so the write is ordered before anything the child
;--- does with x: there is no race to report, and the write is elided. This is
;--- the positive control that the sound rule still elides.
define internal ptr @worker(ptr %a) nounwind {
entry:
  %v = load i32, ptr %a, align 4
  ret ptr null
}

define void @published_via_thread_create() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @published_via_thread_create
; CHECK-NOT:   call void @__tsan_write4(ptr %x)
; CHECK:       ret void
entry:
  %x = alloca i32, align 4
  %t = alloca i64, align 8
  store i32 1, ptr %x, align 4
  br label %later
later:
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr %x)
  ret void
}
