; Every analysis flag with the function pass alone -- no module pass, so no
; module-level analysis result to read. Each must run, not crash, and
; instrument exactly what stock does: with no analysis result the answer is
; "unknown", which is the fail-closed direction. The local escape analysis
; (a function analysis) is the one flag that can elide here, and it takes
; effect in the single call the pass makes. The local is passed to strlen so
; that stock's capture rule (a never-captured alloca is skipped) does not
; already elide it; the local escape analysis knows strlen keeps nothing.

; RUN: opt < %s -passes='function(tsan)' -S | FileCheck %s --check-prefix=STOCK
; RUN: opt < %s -passes='function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s --check-prefix=STOCK
; RUN: opt < %s -passes='function(tsan)' -tsan-use-lock-ownership -S | FileCheck %s --check-prefix=STOCK
; RUN: opt < %s -passes='function(tsan)' -tsan-use-single-threaded -S | FileCheck %s --check-prefix=STOCK
; RUN: opt < %s -passes='function(tsan)' -tsan-use-swmr -S | FileCheck %s --check-prefix=STOCK
; RUN: opt < %s -passes='function(tsan)' -tsan-use-dominance-analysis -S | FileCheck %s --check-prefix=DOM
; RUN: opt < %s -passes='function(tsan)' -tsan-use-escape-analysis -S | FileCheck %s --check-prefix=LOCAL
; RUN: opt < %s -passes='function(tsan)' -tsan-use-escape-analysis -tsan-use-escape-analysis-global -tsan-use-single-threaded -S | FileCheck %s --check-prefix=LOCAL

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g = internal global i32 0, align 4
declare i64 @strlen(ptr)

; STOCK-LABEL: @f
; STOCK: call void @__tsan_write4(ptr %l)
; STOCK: call void @__tsan_write4(ptr @g)
; STOCK: call void @__tsan_write4(ptr @g)
; STOCK: ret void
; DOM-LABEL: @f
; DOM: call void @__tsan_write4(ptr %l)
; DOM: call void @__tsan_write4(ptr @g)
; DOM-NOT: call void @__tsan_write4(ptr @g)
; DOM: ret void
; LOCAL-LABEL: @f
; LOCAL-NOT: call void @__tsan_write4(ptr %l)
; LOCAL: call void @__tsan_write4(ptr @g)
; LOCAL: call void @__tsan_write4(ptr @g)
; LOCAL: ret void
define void @f() sanitize_thread {
  %l = alloca i32, align 4
  %n = call i64 @strlen(ptr %l)
  store i32 1, ptr %l, align 4
  store i32 1, ptr @g, align 4
  store i32 2, ptr @g, align 4
  ret void
}
