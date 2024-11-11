; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8

; void inttoptr_cast() {
; 	long int x;
; 	GPtr = (int *)x;
; }
define void @inttoptr_cast() {
; CHECK: Printing analysis 'Escape Analysis' for function 'inttoptr_cast':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %x = alloca i64, align 8
entry:
  %x = alloca i64, align 8
  %0 = load i64, ptr %x, align 8
  %1 = inttoptr i64 %0 to ptr
  store ptr %1, ptr @GPtr, align 8
  ret void
}

; void ptrtoint_and_inttoptr() {
; 	int y;
; 	int *x = (int *)(long int)&y;
; 	GPtr = x;
; }
define void @ptrtoint_and_inttoptr() #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'ptrtoint_and_inttoptr':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %x = alloca ptr, align 8
; CHECK-DAG:  %y = alloca i32, align 4
entry:
  %y = alloca i32, align 4
  %x = alloca ptr, align 8
  %0 = ptrtoint ptr %y to i64
  %1 = inttoptr i64 %0 to ptr
  store ptr %1, ptr %x, align 8
  %2 = load ptr, ptr %x, align 8
  store ptr %2, ptr @GPtr, align 8
  ret void
}