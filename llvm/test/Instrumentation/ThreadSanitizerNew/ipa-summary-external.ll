; Whole-program escape summary. @sink is defined in unit a and only reads
; through its pointer argument; unit b passes it a local and then writes
; the local. Unit b alone sees an opaque external call and keeps the write.
; The linked program, analysed with -tsan-whole-program, records that
; @sink's argument does not escape; compiled with that summary, unit b
; elides the write. @keep in unit a stores its argument into a global, so
; the write after a call to it stays in every configuration.

; RUN: rm -rf %t && mkdir -p %t && split-file %s %t
; RUN: llvm-link -S %t/a.ll %t/b.ll -o %t/linked.ll
; RUN: opt < %t/linked.ll -passes='print<escape-analysis-global>' -disable-output -tsan-use-analysis-summaries -tsan-whole-program -tsan-summary-dir=%t/sum -tsan-summary-id=t1 2>&1 > /dev/null
; RUN: cat %t/sum/ea_summary.txt | FileCheck %s --check-prefix=FILE
; RUN: opt < %t/b.ll -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -S | FileCheck %s --check-prefix=ALONE
; RUN: opt < %t/b.ll -passes='module(tsan-module),function(tsan)' -tsan-use-escape-analysis-global -tsan-use-analysis-summaries -tsan-summary-dir=%t/sum -tsan-summary-id=t1 -S | FileCheck %s --check-prefix=SEEDED
; RUN: cat %t/sum/ea_summary.txt | FileCheck %s --check-prefix=FILE

; FILE: # tsan-summary-id: t1
; FILE: sink: 0
; FILE-NOT: keep
; ALONE-LABEL: @through_sink
; ALONE: call void @__tsan_write4
; SEEDED-LABEL: @through_sink
; SEEDED-NOT: call void @__tsan_write4
; SEEDED: ret void
; SEEDED-LABEL: @through_keep
; SEEDED: call void @__tsan_write4

;--- a.ll
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
@slot = global ptr null, align 8
define i32 @sink(ptr %p) {
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
define void @keep(ptr %p) {
  store ptr %p, ptr @slot, align 8
  ret void
}

;--- b.ll
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
declare i32 @sink(ptr)
declare void @keep(ptr)
define void @through_sink() sanitize_thread {
  %l = alloca i32, align 4
  store i32 1, ptr %l, align 4
  %r = call i32 @sink(ptr %l)
  store i32 2, ptr %l, align 4
  ret void
}
define void @through_keep() sanitize_thread {
  %l = alloca i32, align 4
  store i32 1, ptr %l, align 4
  call void @keep(ptr %l)
  store i32 2, ptr %l, align 4
  ret void
}
