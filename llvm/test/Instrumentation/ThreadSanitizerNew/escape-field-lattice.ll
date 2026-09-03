; The first two cases were also elided by stock TSan, and by upstream LLVM: its
; capture-tracking step asked whether the address value -- a GEP whose only
; use is the access -- may be captured, not whether the variable may be. That
; is fixed here for every configuration, so the STOCK run pins it too.
;
; Which recorded escape covers which access. Escapes and accesses are keyed by
; (object, field path); the lookup was an exact match, so an object that
; escaped as a whole -- {s,[]} -- did not cover an access to one of its fields,
; {s,[1]}, and the field write after foo(&s) was elided. The rule now: a path
; covers every path it is a prefix of, and an escaped field covers the whole
; object (an access to the whole touches that field). Incomparable fields stay
; independent -- that is the field-sensitivity the paper claims, and the last
; function keeps it.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s --check-prefixes=CHECK,EA
; RUN: opt < %s -passes='function(tsan)' -S | FileCheck %s --check-prefixes=CHECK,STOCK

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@sink = global ptr null, align 8
declare void @foo(ptr)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

; Whole object escaped, field accessed.
define void @whole_then_field() sanitize_thread {
; CHECK-LABEL: @whole_then_field
; CHECK:       call void @__tsan_write4(ptr %f1)
entry:
  %s = alloca { i32, i32 }, align 8
  call void @foo(ptr %s)
  %f1 = getelementptr inbounds { i32, i32 }, ptr %s, i64 0, i32 1
  store i32 1, ptr %f1, align 4
  ret void
}

; Field escaped, whole object accessed (the copy reads the escaped field).
define void @field_then_whole(ptr %dst) sanitize_thread {
; CHECK-LABEL: @field_then_whole
; CHECK:       call void @__tsan_write4(ptr %f1)
entry:
  %s = alloca { i32, i32 }, align 8
  %f1 = getelementptr inbounds { i32, i32 }, ptr %s, i64 0, i32 1
  store ptr %f1, ptr @sink, align 8
  store i32 1, ptr %f1, align 4
  ret void
}

; Field 1 escaped, field 2 accessed: independent, still elided by the
; field-sensitive analysis; stock has no field sensitivity and keeps it.
define void @other_field_stays_local() sanitize_thread {
; CHECK-LABEL: @other_field_stays_local
; EA-NOT:      call void @__tsan_write4(ptr %f2)
; STOCK:       call void @__tsan_write4(ptr %f2)
; CHECK:       ret void
entry:
  %s = alloca { i32, i32, i32 }, align 8
  %f1 = getelementptr inbounds { i32, i32, i32 }, ptr %s, i64 0, i32 1
  store ptr %f1, ptr @sink, align 8
  %f2 = getelementptr inbounds { i32, i32, i32 }, ptr %s, i64 0, i32 2
  store i32 1, ptr %f2, align 4
  ret void
}
