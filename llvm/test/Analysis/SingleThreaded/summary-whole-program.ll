; Whole-program summaries. The linked program is analysed once with
; -tsan-whole-program (external linkage does not by itself mean unknown
; callers or writers) and writes a summary keyed only by externally visible
; names; a unit compiled on its own reads it and overlays those verdicts on
; its per-unit analysis. Here @helper (defined in unit b, called only from
; main in unit a before any thread exists) is single-threaded in the whole
; program though unit b cannot know it, and @cfg (written by main before
; the thread, read by the worker) is single-writer. @bad is written by the
; worker and must not appear. A summary written under another
; -tsan-summary-id is ignored with a warning, and a static in unit b that
; shares a name with an external of unit a is untouched.

; RUN: rm -rf %t && mkdir -p %t && split-file %s %t
; RUN: llvm-link -S %t/a.ll %t/b.ll -o %t/linked.ll
; RUN: opt < %t/linked.ll -passes='print<single-threaded>' -disable-output -tsan-use-analysis-summaries -tsan-whole-program -tsan-summary-dir=%t/sum -tsan-summary-id=t1 2>&1 | FileCheck %s --check-prefix=LINKED
; RUN: cat %t/sum/st_summary.txt | FileCheck %s --check-prefix=FILE
; RUN: opt < %t/b.ll -passes='print<single-threaded>' -disable-output 2>&1 | FileCheck %s --check-prefix=ALONE
; RUN: opt < %t/b.ll -passes='print<single-threaded>' -disable-output -tsan-use-analysis-summaries -tsan-summary-dir=%t/sum -tsan-summary-id=t1 2>&1 | FileCheck %s --check-prefix=SEEDED
; RUN: opt < %t/b.ll -passes='print<single-threaded>' -disable-output -tsan-use-analysis-summaries -tsan-summary-dir=%t/sum -tsan-summary-id=t2 2>&1 | FileCheck %s --check-prefix=STALE

; LINKED-LABEL: Single-Threaded Functions
; LINKED: helper
; FILE: # tsan-summary-id: t1
; FILE-NEXT: # tsan-summary-flags: whole-program=1
; FILE: --- Single-Threaded Functions ---
; FILE-NEXT: helper
; FILE-NOT: local_only
; FILE: --- Read-Only Global Variables
; FILE-NEXT: cfg
; FILE-NOT: bad
; ALONE-LABEL: Single-Threaded Functions
; ALONE-NOT: helper
; ALONE-LABEL: Unclassified Functions
; ALONE: helper
; ALONE-LABEL: Read-Only Globals
; ALONE-NOT: cfg
; SEEDED-LABEL: Single-Threaded Functions
; SEEDED: helper
; SEEDED-LABEL: Unclassified Functions
; SEEDED-NOT: helper
; SEEDED-LABEL: Read-Only Globals
; SEEDED: cfg
; SEEDED-NOT: bad
; STALE: warning: ignoring st_summary.txt: written with summary id 't1', this compile uses 't2'
; STALE-LABEL: Single-Threaded Functions
; STALE-NOT: helper
; STALE-LABEL: Unclassified Functions

;--- a.ll
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
@cfg = global i32 0, align 4
@bad = global i32 0, align 4
declare i32 @pthread_create(ptr, ptr, ptr, ptr)
declare void @helper()
declare i32 @reader()
define internal ptr @worker(ptr %a) {
  store i32 1, ptr @bad, align 4
  %r = call i32 @reader()
  ret ptr null
}
define i32 @main() {
  %t = alloca i64, align 8
  store i32 7, ptr @cfg, align 4
  call void @helper()
  %c = call i32 @pthread_create(ptr %t, ptr null, ptr @worker, ptr null)
  ret i32 0
}

;--- b.ll
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
@cfg = external global i32, align 4
@bad = external global i32, align 4
define void @helper() {
  %v = load i32, ptr @cfg, align 4
  ret void
}
define internal void @local_only() {
  ret void
}
define i32 @reader() {
  %v = load i32, ptr @cfg, align 4
  %w = load i32, ptr @bad, align 4
  call void @local_only()
  ret i32 %v
}
