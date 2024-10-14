; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8
@GV = dso_local global i32 0, align 4

define dso_local void @escape_global() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_global':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  store i32 20, ptr %x, align 4
  store ptr %x, ptr @GPtr, align 8
  ret void
}

define dso_local void @escape_global_variable() #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_global_variable':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:  %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  %0 = load i32, ptr %x, align 4
  store i32 %0, ptr @GV, align 4
  ret void
}