; Two things the single-threaded analysis must get right at its roots:
; __kmpc_fork_teams is a thread creator like the others, and main is
; classified per block -- the blocks before any creation on every path are
; single-threaded, the blocks after a creation multi-threaded, and a block
; reachable both with and without a creation is multi-threaded.

; RUN: opt < %s -passes='print<single-threaded>' -disable-output 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

@g = internal global i32 0, align 4
declare void @__kmpc_fork_teams(ptr, i32, ptr, ...)
define internal void @omp_outlined(ptr %a, ptr %b) {
  store i32 1, ptr @g, align 4
  ret void
}
define internal void @spawn_teams() {
  call void (ptr, i32, ptr, ...) @__kmpc_fork_teams(ptr null, i32 0, ptr @omp_outlined)
  ret void
}
define internal void @before_only() {
  store i32 0, ptr @g, align 4
  ret void
}
define i32 @main(i32 %argc) {
entry:
  call void @before_only()
  %c = icmp eq i32 %argc, 1
  br i1 %c, label %spawn, label %join
spawn:
  call void @spawn_teams()
  br label %join
join:
  store i32 2, ptr @g, align 4
  ret i32 0
}

; CHECK-LABEL: Thread Creator Functions
; CHECK: spawn_teams
; CHECK-LABEL: Multi-Threaded Functions
; CHECK: omp_outlined
; CHECK-LABEL: Single-Threaded Functions
; CHECK: before_only
