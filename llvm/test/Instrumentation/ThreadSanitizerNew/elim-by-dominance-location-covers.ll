; What "same location" means for a cover: the covering access must be at
; least as wide as the covered one (a wider store covers a narrower one at
; the same address, not the reverse), and an access of scalable size is
; never a cover nor covered -- its extent is unknown at compile time (the
; pass does not instrument scalable accesses at all, so the bail is a guard
; behind that upstream decision).

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-dom -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i64 0, align 8

; CHECK-LABEL: @wide_covers_narrow
; CHECK: call void @__tsan_write8(ptr @x)
; CHECK-NOT: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @wide_covers_narrow() sanitize_thread {
  store i64 1, ptr @x, align 8
  store i32 2, ptr @x, align 4
  ret void
}

; CHECK-LABEL: @narrow_does_not_cover_wide
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK: call void @__tsan_write8(ptr @x)
; CHECK: ret void
define void @narrow_does_not_cover_wide() sanitize_thread {
  store i32 1, ptr @x, align 4
  store i64 2, ptr @x, align 8
  ret void
}

; CHECK-LABEL: @scalable_bails
; CHECK-NOT: call void @__tsan_
; CHECK: ret void
define void @scalable_bails(ptr %p, <vscale x 4 x i32> %v) sanitize_thread {
  store <vscale x 4 x i32> %v, ptr %p, align 4
  store <vscale x 4 x i32> %v, ptr %p, align 4
  ret void
}
