; What counts as starting a thread. Each of these reaches pthread_create inside
; a runtime library, so what a translation unit sees is the wrapper; recognising
; only the literal pthread_create left whole programs looking single-threaded.

; RUN: opt < %s -passes='print<single-threaded>' -disable-output 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

declare i32 @pthread_create(ptr, ptr, ptr, ptr)
declare i32 @thrd_create(ptr, ptr, ptr)
declare void @__kmpc_fork_call(ptr, i32, ptr, ...)
declare ptr @__tsan_create_fiber(i32)
declare void @_ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE(ptr, ptr, ptr)

define internal void @spawn_pthread() nounwind {
  %t = alloca i64, align 8
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr null, ptr null)
  ret void
}
define internal void @spawn_c11() nounwind {
  %t = alloca i64, align 8
  %c = call i32 @thrd_create(ptr %t, ptr null, ptr null)
  ret void
}
define internal void @spawn_openmp() nounwind {
  call void (ptr, i32, ptr, ...) @__kmpc_fork_call(ptr null, i32 0, ptr null)
  ret void
}
define internal void @spawn_fiber() nounwind {
  %f = call ptr @__tsan_create_fiber(i32 0)
  ret void
}
define internal void @spawn_cxx() nounwind {
  call void @_ZNSt6thread15_M_start_threadESt10unique_ptrINS_6_StateESt14default_deleteIS1_EEPFvvE(ptr null, ptr null, ptr null)
  ret void
}

; Called before anything is started, and starts nothing itself.
define internal void @before_any_thread() nounwind {
  ret void
}

define i32 @main() nounwind {
entry:
  call void @before_any_thread()
  call void @spawn_pthread()
  call void @spawn_c11()
  call void @spawn_openmp()
  call void @spawn_fiber()
  call void @spawn_cxx()
  ret i32 0
}

; CHECK: Thread Creator Functions
; CHECK-DAG: spawn_pthread
; CHECK-DAG: spawn_c11
; CHECK-DAG: spawn_openmp
; CHECK-DAG: spawn_fiber
; CHECK-DAG: spawn_cxx
; CHECK: Single-Threaded Functions
; CHECK: before_any_thread
