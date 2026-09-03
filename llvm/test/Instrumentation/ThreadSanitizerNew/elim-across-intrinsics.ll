; Which intrinsics an access may be eliminated across.
;
; This used to be a hand-written blocklist ending in "everything else is
; synchronization-free". That list names no target intrinsics, so a memory
; fence and a thread barrier both counted as sync-free and elimination went
; straight through them. The answer now comes from what the intrinsic declares:
; touching no memory, or only the memory passed in as arguments, means there is
; nothing to synchronize through; anything else is assumed to synchronize.
;
; The last two functions are the controls. Without them a change that simply
; stopped eliminating across every intrinsic would satisfy the first CHECK.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-dom -S | FileCheck %s
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -S | FileCheck %s --check-prefix=NODOM

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g = global i32 0, align 4

declare void @llvm.x86.sse2.mfence()
declare float @llvm.fabs.f32(float)
declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 immarg)

;--- A memory fence orders this thread against others. Nothing crosses it.
define void @across_mfence() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @across_mfence
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       call void @llvm.x86.sse2.mfence()
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @across_mfence
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
  store i32 1, ptr @g, align 4
  call void @llvm.x86.sse2.mfence()
  store i32 2, ptr @g, align 4
  ret void
}

;--- An intrinsic that touches no memory has nothing to synchronize through.
define void @across_fabs(float %x) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @across_fabs
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK-NOT:   call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @across_fabs
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
  store i32 1, ptr @g, align 4
  %r = call float @llvm.fabs.f32(float %x)
  store i32 2, ptr @g, align 4
  ret void
}

;--- Nor does one that touches only what it was handed. This is the case that
;--- rules out answering the question with the nosync attribute alone: memset
;--- does not carry it, and treating it as synchronizing would cost most of
;--- what the analysis achieves.
define void @across_memset(ptr %p) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @across_memset
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK-NOT:   call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NODOM-LABEL: @across_memset
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       call void @__tsan_write4(ptr @g)
; NODOM:       ret void
  store i32 1, ptr @g, align 4
  call void @llvm.memset.p0.i64(ptr %p, i8 0, i64 16, i1 false)
  store i32 2, ptr @g, align 4
  ret void
}
