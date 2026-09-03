; Post-dominance elimination removes the earlier of two accesses when the
; later one is always reached; a call between them that may never return
; breaks that. Library functions used to be exempt from the termination
; test as long as they were sync-free: strlen without any attribute passed,
; although nothing says an arbitrary library call returns. Now a library
; call blocks post-dominance unless it is willreturn (the attribute
; inference passes put it there for the functions that provably return);
; dominance, which needs no termination, still crosses it (DOM).

; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-postdom -S | FileCheck %s
; RUN: opt < %s -passes='module(tsan-module),function(tsan)' -tsan-use-dominance-analysis-dom -S | FileCheck %s --check-prefix=DOM

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@x = global i32 0, align 4
declare i64 @strlen(ptr)

; No attribute: may not return, so post-dominance keeps both stores.
; CHECK-LABEL: @bare
; CHECK-COUNT-2: call void @__tsan_write4(ptr @x)
; CHECK: ret void
; DOM-LABEL: @bare
; DOM: call void @__tsan_write4(ptr @x)
; DOM-NOT: call void @__tsan_write4(ptr @x)
; DOM: ret void
define void @bare(ptr %s) sanitize_thread {
  store i32 1, ptr @x, align 4
  %n = call i64 @strlen(ptr %s)
  store i32 2, ptr @x, align 4
  ret void
}

; willreturn on the call: the later store is always reached, the earlier
; store is redundant under post-dominance.
; CHECK-LABEL: @returns
; CHECK: call void @__tsan_write4(ptr @x)
; CHECK-NOT: call void @__tsan_write4(ptr @x)
; CHECK: ret void
define void @returns(ptr %s) sanitize_thread {
  store i32 1, ptr @x, align 4
  %n = call i64 @strlen(ptr %s) willreturn
  store i32 2, ptr @x, align 4
  ret void
}
