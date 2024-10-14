; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

%struct.StructTy = type { ptr }

@GPtr = dso_local global ptr null, align 8

define dso_local ptr @escape_aliasing() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_aliasing':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK-DAG:   %alias = alloca ptr, align 8
entry:
  %x = alloca i32, align 4
  %alias = alloca ptr, align 8
  store i32 50, ptr %x, align 4
  store ptr %x, ptr %alias, align 8
  %0 = load ptr, ptr %alias, align 8
  store ptr %0, ptr @GPtr, align 8
  ret ptr %x
}

define dso_local void @multiple_aliases() {
; CHECK: Printing analysis 'Escape Analysis' for function 'multiple_aliases':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %z = alloca i32, align 4
; CHECK-DAG:   %Ptr = alloca ptr, align 8
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  %z = alloca i32, align 4
  %Ptr = alloca ptr, align 8
  store ptr %x, ptr %Ptr, align 8
  store ptr %y, ptr %Ptr, align 8
  store ptr %z, ptr %Ptr, align 8
  %0 = load ptr, ptr %Ptr, align 8
  store ptr %0, ptr @GPtr, align 8
  ret void
}