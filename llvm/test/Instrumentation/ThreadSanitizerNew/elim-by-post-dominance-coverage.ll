; Which access can stand in for which, when the covering one comes later.
;
; TSan already drops a read that is followed by a write to the same address in
; the same basic block. Post-dominance is the general form of that rule: the
; covering write need only post-dominate the read, and the path between them is
; checked rather than assumed. These cases pin down the coverage rule in both
; directions.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-postdom -S | FileCheck %s
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -S | FileCheck %s --check-prefix=NOPOSTDOM

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g = global i32 0, align 4
@s = global i32 0, align 4

;--- A later write covers an earlier read, across blocks. This is
;--- read-before-write generalised past the end of a basic block.
define void @write_covers_earlier_read(i1 %c) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @write_covers_earlier_read
; CHECK-NOT:   call void @__tsan_read4(ptr @g)
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       ret void
; NOPOSTDOM-LABEL: @write_covers_earlier_read
; NOPOSTDOM:       call void @__tsan_read4(ptr @g)
; NOPOSTDOM:       call void @__tsan_write4(ptr @g)
; NOPOSTDOM:       ret void
entry:
  %v = load i32, ptr @g, align 4
  br i1 %c, label %a, label %b
a:
  store i32 %v, ptr @s, align 4
  br label %join
b:
  br label %join
join:
  store i32 42, ptr @g, align 4
  ret void
}

;--- A later read covers an earlier read: a read races only with a remote
;--- write, and the later read still finds it.
define void @read_covers_earlier_read(i1 %c) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @read_covers_earlier_read
; CHECK-NOT:   call void @__tsan_read4(ptr @g)
; CHECK:       call void @__tsan_read4(ptr @g)
; CHECK:       ret void
; NOPOSTDOM-LABEL: @read_covers_earlier_read
; NOPOSTDOM:       call void @__tsan_read4(ptr @g)
; NOPOSTDOM:       call void @__tsan_read4(ptr @g)
; NOPOSTDOM:       ret void
entry:
  %v = load i32, ptr @g, align 4
  store i32 %v, ptr @s, align 4
  br i1 %c, label %a, label %b
a:
  br label %join
b:
  br label %join
join:
  %w = load i32, ptr @g, align 4
  store i32 %w, ptr @s, align 4
  ret void
}

;--- A later read cannot cover an earlier write. The write may race with a
;--- remote read, which the covering read would never flag.
define void @read_does_not_cover_earlier_write(i1 %c) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @read_does_not_cover_earlier_write
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       call void @__tsan_read4(ptr @g)
; CHECK:       ret void
; NOPOSTDOM-LABEL: @read_does_not_cover_earlier_write
; NOPOSTDOM:       call void @__tsan_write4(ptr @g)
; NOPOSTDOM:       call void @__tsan_read4(ptr @g)
; NOPOSTDOM:       ret void
entry:
  store i32 7, ptr @g, align 4
  br i1 %c, label %a, label %b
a:
  br label %join
b:
  br label %join
join:
  %w = load i32, ptr @g, align 4
  store i32 %w, ptr @s, align 4
  ret void
}
