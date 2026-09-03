; Two ways a global was called protected without evidence.
;
; Only Instruction users of a global were examined, so an access through a
; constant getelementptr -- which is how every access to a field of an aggregate
; global is written -- was invisible here, while the instrumentation pass still
; resolved it back to the global and acted on the verdict.
;
; And the acceptance test admitted a global whose accesses had all been skipped:
; "all accesses protected" is vacuously true when none was looked at.

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --implicit-check-not='* rec' --implicit-check-not='* st_only'

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@mtx = global i64 0, align 8
@control = global i32 0, align 4
@rec = global { i32, i32 } zeroinitializer, align 8
@st_only = global i32 0, align 4

declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)

define internal ptr @worker(ptr %arg) nounwind {
entry:
  %l = call i32 @pthread_mutex_lock(ptr @mtx)
  store i32 1, ptr @control, align 4
  ; field 0, under the lock
  store i32 1, ptr @rec, align 8
  %u = call i32 @pthread_mutex_unlock(ptr @mtx)
  ; field 1 through a constant getelementptr, with nothing held
  store i32 2, ptr getelementptr inbounds ({ i32, i32 }, ptr @rec, i64 0, i32 1), align 4
  ret ptr null
}

; Reached only from the single-threaded prefix of main, so its accesses are
; skipped -- which must leave @st_only unproven, not proven.
define internal void @before_threads() nounwind {
entry:
  store i32 7, ptr @st_only, align 4
  ret void
}

define i32 @main() nounwind {
entry:
  %t = alloca i64, align 8
  call void @before_threads()
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr null)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK: * control
