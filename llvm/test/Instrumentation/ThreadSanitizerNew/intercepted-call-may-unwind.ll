; The interceptor toggle around strlen/strcmp/memchr on a provably local
; buffer clears InterceptorEnabled before the call and restores it in the
; instruction after. If the call can unwind, the instruction after is never
; reached and the thread runs the rest of the program with interceptors
; off: every later intercepted access is lost. So the toggle is emitted
; only for a call that cannot unwind (nounwind on the call or the callee).

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

declare i64 @strlen(ptr)
declare i64 @strlen_nounwind(ptr) nounwind

; CHECK-LABEL: @may_unwind
; CHECK-NOT: store i1 false, ptr @InterceptorEnabled
; CHECK: {{call|invoke}} i64 @strlen(
; CHECK-NOT: store i1 {{.*}}, ptr @InterceptorEnabled
; CHECK: ret i64
define i64 @may_unwind() sanitize_thread {
  %b = alloca [8 x i8], align 1
  store i8 0, ptr %b, align 1
  %n = call i64 @strlen(ptr %b)
  ret i64 %n
}

; CHECK-LABEL: @call_nounwind
; CHECK: store i1 false, ptr @InterceptorEnabled
; CHECK: call i64 @strlen(
; CHECK: store i1 %{{.*}}, ptr @InterceptorEnabled
; CHECK: ret i64
define i64 @call_nounwind() sanitize_thread {
  %b = alloca [8 x i8], align 1
  store i8 0, ptr %b, align 1
  %n = call i64 @strlen(ptr %b) nounwind
  ret i64 %n
}
