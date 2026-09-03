; Each translation unit is compiled alone. A function another unit can name
; may be called from a thread created there, so it is single-threaded only if
; it has local linkage, its address is never taken, and this unit calls it
; only before threads exist. The analysis defaulted every function it had not
; seen called to single-threaded.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-single-threaded -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g = global i32 0, align 4
@table = internal global [1 x ptr] [ptr @taken]

; External linkage, never called here: some other unit may, from a thread.
define void @exported() sanitize_thread {
; CHECK-LABEL: @exported
; CHECK:       call void @__tsan_write4(ptr @g)
  store i32 1, ptr @g, align 4
  ret void
}

; linkonce_odr: the linker may keep another unit's copy, or a thread there may call this one.
define linkonce_odr void @inl() sanitize_thread {
; CHECK-LABEL: @inl
; CHECK:       call void @__tsan_write4(ptr @g)
  store i32 2, ptr @g, align 4
  ret void
}

; Internal, but its address sits in a table: reachable from anywhere that reads the table.
define internal void @taken() sanitize_thread {
; CHECK-LABEL: @taken
; CHECK:       call void @__tsan_write4(ptr @g)
  store i32 3, ptr @g, align 4
  ret void
}

; Internal, direct calls only, called from main before any thread: single-threaded.
define internal void @local() sanitize_thread {
; CHECK-LABEL: @local
; CHECK-NOT:   call void @__tsan_write4(ptr @g)
; CHECK:       ret void
  store i32 4, ptr @g, align 4
  ret void
}

define i32 @main() sanitize_thread {
  call void @local()
  ret i32 0
}
