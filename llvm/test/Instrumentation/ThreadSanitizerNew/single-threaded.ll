; The single-threaded-context analysis skips accesses that cannot yet race
; because no other thread exists. main is the interesting case: it begins
; single-threaded and usually stops being so part-way through, so it has to be
; classified one access at a time rather than as a whole.

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-single-threaded -S | FileCheck %s
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -S | FileCheck %s --check-prefix=NOSTC

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g = global i32 0, align 4
@h = global i32 0, align 4

declare i32 @pthread_create(ptr, ptr, ptr, ptr)
declare i32 @pthread_join(i64, ptr)
declare void @_ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE(ptr, ptr, ptr)

; A thread body. Its address is taken, so it runs multi-threaded.
define internal ptr @worker(ptr %arg) nounwind uwtable sanitize_thread {
; CHECK-LABEL: @worker
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       ret ptr
  store i32 1, ptr @g, align 4
  ret ptr null
}

; The write before pthread_create runs while the program is still
; single-threaded and is skipped; the one after it is not, and dropping it
; loses every race between main and the thread it just started.
define i32 @main() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @main
; CHECK-NOT:   call void @__tsan_write4(ptr @h)
; CHECK:       call i32 @pthread_create
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       ret i32 0
; NOSTC-LABEL: @main
; NOSTC:       call void @__tsan_write4(ptr @h)
; NOSTC:       call i32 @pthread_create
; NOSTC:       call void @__tsan_write4(ptr @g)
; NOSTC:       ret i32 0
entry:
  %t = alloca i64, align 8
  store i32 7, ptr @h, align 4
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr null)
  store i32 2, ptr @g, align 4
  %tid = load i64, ptr %t, align 8
  %j = call i32 @pthread_join(i64 %tid, ptr null)
  ret i32 0
}

; std::thread reaches pthread_create inside libstdc++, so the only call this
; module can see is the wrapper. If that is not recognised as starting a
; thread, everything after it looks single-threaded.
define i32 @cxx_thread_main() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @cxx_thread_main
; CHECK:       call void @_ZNSt6thread15_M_start_thread
; CHECK:       call void @__tsan_write4(ptr @g)
; CHECK:       ret i32 0
entry:
  call void @_ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE(ptr null, ptr null, ptr @worker)
  store i32 3, ptr @g, align 4
  ret i32 0
}

; A function only ever reached from the single-threaded prefix keeps being
; skipped wholesale -- the point of the analysis is preserved.
define internal void @only_called_before_threads() nounwind uwtable sanitize_thread {
; CHECK-LABEL: @only_called_before_threads
; CHECK-NOT:   call void @__tsan_write4
; CHECK:       ret void
; NOSTC-LABEL: @only_called_before_threads
; NOSTC:       call void @__tsan_write4(ptr @h)
; NOSTC:       ret void
  store i32 5, ptr @h, align 4
  ret void
}
