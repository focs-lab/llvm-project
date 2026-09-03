; The measurement-only switches must run and stay conservative:
; -tsan-use-lock-ownership-upperbound (every access under any lock counts
; as protected -- an upper bound on what lock ownership could remove,
; documented unsound), -tsan-stc-skip-func-entry-exit (a single-threaded
; function also loses __tsan_func_entry/exit), -tsan-attribute-flow-
; sensitivity (attribution counters only; the elision decision is the
; sound rule's).

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-lock-ownership-upperbound -S | FileCheck %s --check-prefix=UPPER
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-single-threaded -tsan-stc-skip-func-entry-exit -S | FileCheck %s --check-prefix=SKIP
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -tsan-attribute-flow-sensitivity -S | FileCheck %s --check-prefix=ATTR

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@m = global i64 0, align 8
@g = internal global i32 0, align 4
declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)
declare void @opaque(ptr)

; UPPER-LABEL: @under_lock
; UPPER-NOT: call void @__tsan_write4
; UPPER: ret ptr
define internal ptr @under_lock(ptr %a) sanitize_thread {
  %l = call i32 @pthread_mutex_lock(ptr @m)
  store i32 1, ptr @g, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @m)
  ret ptr null
}

; SKIP-LABEL: @only_from_main
; SKIP-NOT: __tsan_func_entry
; SKIP-NOT: __tsan_write4
; SKIP: ret void
define internal void @only_from_main() sanitize_thread {
  store i32 2, ptr @g, align 4
  ret void
}

; ATTR-LABEL: @published_later
; ATTR: call void @__tsan_write4(ptr %l)
define void @published_later() sanitize_thread {
entry:
  %l = alloca i32, align 4
  store i32 1, ptr %l, align 4
  br label %pub
pub:
  call void @opaque(ptr %l)
  ret void
}

define i32 @main() sanitize_thread {
  %t = alloca i64, align 8
  call void @only_from_main()
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @under_lock, ptr null)
  ret i32 0
}
