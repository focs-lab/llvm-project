; Dominance elimination may only drop an access that another access provably
; covers: the same address, and at least as many bytes. These are the cases
; where "same underlying object" is not the same thing as "same location".
;
; Every elimination below is paired with a NODOM run over the same function, so
; a CHECK-NOT that stops matching because instrumentation moved -- rather than
; because it was correctly removed -- still fails the test.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-dom -S | FileCheck %s
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -S | FileCheck %s --check-prefix=NODOM

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@arr = global [8 x i32] zeroinitializer, align 16
@rec = global { i32, i32 } zeroinitializer, align 8
@gv = global i64 0, align 8

;--- Distinct array elements are distinct locations.
; A write to arr[0] says nothing about a race on arr[3]; collapsing the two
; because they share the underlying object @arr loses that race entirely.
define void @distinct_array_elements() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @distinct_array_elements
; CHECK:       call void @__tsan_write4(ptr @arr)
; CHECK:       call void @__tsan_write4(ptr getelementptr
; CHECK:       ret void
; NODOM-LABEL: @distinct_array_elements
; NODOM:       call void @__tsan_write4(ptr @arr)
; NODOM:       call void @__tsan_write4(ptr getelementptr
; NODOM:       ret void
entry:
  store i32 1, ptr @arr, align 16
  store i32 2, ptr getelementptr inbounds ([8 x i32], ptr @arr, i64 0, i64 3), align 4
  ret void
}

;--- Distinct fields of one global are distinct locations.
define void @distinct_struct_fields() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @distinct_struct_fields
; CHECK:       call void @__tsan_write4(ptr @rec)
; CHECK:       call void @__tsan_write4(ptr getelementptr
; CHECK:       ret void
; NODOM-LABEL: @distinct_struct_fields
; NODOM:       call void @__tsan_write4(ptr @rec)
; NODOM:       call void @__tsan_write4(ptr getelementptr
; NODOM:       ret void
entry:
  store i32 1, ptr @rec, align 8
  store i32 2, ptr getelementptr inbounds ({ i32, i32 }, ptr @rec, i64 0, i32 1), align 4
  ret void
}

;--- The same element twice is one location: elimination is expected here, and
;--- this is what keeps the two tests above honest.
define void @same_array_element() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @same_array_element
; CHECK:       call void @__tsan_write4(ptr getelementptr
; CHECK-NOT:   call void @__tsan_write4
; CHECK:       ret void
; NODOM-LABEL: @same_array_element
; NODOM:       call void @__tsan_write4(ptr getelementptr
; NODOM:       call void @__tsan_write4(ptr getelementptr
; NODOM:       ret void
entry:
  store i32 1, ptr getelementptr inbounds ([8 x i32], ptr @arr, i64 0, i64 3), align 4
  store i32 2, ptr getelementptr inbounds ([8 x i32], ptr @arr, i64 0, i64 3), align 4
  ret void
}

;--- A narrow access does not cover a wider one at the same address. The four
;--- bytes above the first word are unprotected if the 8-byte store is dropped.
define void @narrow_does_not_cover_wide() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @narrow_does_not_cover_wide
; CHECK:       call void @__tsan_write4(ptr @gv)
; CHECK:       call void @__tsan_write8(ptr @gv)
; CHECK:       ret void
; NODOM-LABEL: @narrow_does_not_cover_wide
; NODOM:       call void @__tsan_write4(ptr @gv)
; NODOM:       call void @__tsan_write8(ptr @gv)
; NODOM:       ret void
entry:
  store i32 1, ptr @gv, align 8
  store i64 2, ptr @gv, align 8
  ret void
}

;--- A wide access does cover a narrower one at the same address.
define void @wide_covers_narrow() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @wide_covers_narrow
; CHECK:       call void @__tsan_write8(ptr @gv)
; CHECK-NOT:   call void @__tsan_write4
; CHECK:       ret void
; NODOM-LABEL: @wide_covers_narrow
; NODOM:       call void @__tsan_write8(ptr @gv)
; NODOM:       call void @__tsan_write4(ptr @gv)
; NODOM:       ret void
entry:
  store i64 1, ptr @gv, align 8
  store i32 2, ptr @gv, align 8
  ret void
}

;--- A dominating read cannot stand in for a later write: the write may race
;--- with a remote read, which the read would not have flagged. The two are in
;--- separate blocks because within one block stock TSan drops the read anyway,
;--- which would hide whatever dominance elimination did.
define void @read_does_not_cover_write(i1 %c) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @read_does_not_cover_write
; CHECK:       call void @__tsan_read4(ptr @arr)
; CHECK:       call void @__tsan_write4(ptr @arr)
; CHECK:       ret void
; NODOM-LABEL: @read_does_not_cover_write
; NODOM:       call void @__tsan_read4(ptr @arr)
; NODOM:       call void @__tsan_write4(ptr @arr)
; NODOM:       ret void
entry:
  %v = load i32, ptr @arr, align 16
  br label %next

next:
  store i32 %v, ptr @arr, align 16
  ret void
}
