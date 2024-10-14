; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@global_ptr = dso_local global ptr null, align 8

define dso_local ptr @escape_local() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_local':
; CHECK-NEXT: Escaping variables:
; CHECK-NEXT:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  ret ptr %x
}

define dso_local ptr @escape_by_returning_ptr() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_by_returning_ptr':
; CHECK-NEXT: Escaping variables:
; CHECK-NEXT:   %x = alloca ptr, align 8
entry:
  %x = alloca ptr, align 8
  %0 = load ptr, ptr %x, align 8
  ret ptr %0
}