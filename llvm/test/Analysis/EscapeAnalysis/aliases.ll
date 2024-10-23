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

define dso_local void @global_variable_as_alias_in_GEP() {
; CHECK: Printing analysis 'Escape Analysis' for function 'global_variable_as_alias_in_GEP':
; CHECK-NOT: Escaping variables:
entry:
  %.atomictmp = alloca i32, align 4
  %0 = load ptr, ptr @GPtr, align 8
  %arrayidx = getelementptr inbounds i32, ptr %0, i64 0
  store i32 333, ptr %.atomictmp, align 4
  %1 = load i32, ptr %.atomictmp, align 4
  store atomic i32 %1, ptr %arrayidx release, align 4
  ret void
}

define dso_local void @escape_pointee_object() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_pointee_object':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %p = alloca ptr, align 8
; CHECK-DAG:   %x = alloca [10 x i32], align 16
entry:
  %x = alloca [10 x i32], align 16
  %p = alloca ptr, align 8
  %arrayidx = getelementptr inbounds [10 x i32], ptr %x, i64 0, i64 5
  store ptr %arrayidx, ptr %p, align 8
  %arrayidx1 = getelementptr inbounds [10 x i32], ptr %x, i64 0, i64 2
  store ptr %arrayidx1, ptr @GPtr, align 8
  ret void
}

define dso_local void @escape_in_the_middle_of_alias_chain() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_in_the_middle_of_alias_chain':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:  %z = alloca ptr, align 8
; CHECK-DAG:  %y = alloca ptr, align 8
; CHECK-DAG:  %x = alloca [10 x i32], align 16
entry:
  %x = alloca [10 x i32], align 16
  %y = alloca ptr, align 8
  %z = alloca ptr, align 8
  %arrayidx = getelementptr inbounds [10 x i32], ptr %x, i64 0, i64 3
  store ptr %arrayidx, ptr %y, align 8
  %0 = load ptr, ptr %y, align 8
  %arrayidx1 = getelementptr inbounds i32, ptr %0, i64 4
  store ptr %arrayidx1, ptr %z, align 8
  %1 = load ptr, ptr %y, align 8
  store ptr %1, ptr @GPtr, align 8
  ret void
}