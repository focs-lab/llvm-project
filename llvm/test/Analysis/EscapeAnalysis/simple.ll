; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

%struct.StructTy = type { ptr }

@GPtr = dso_local global ptr null, align 8

define dso_local void @escape_func() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_func':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-NEXT:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  store i32 333, ptr %x, align 4
  call void @external_func(ptr noundef %x)
  ret void
}

declare void @external_func(ptr noundef)

define dso_local void @assigning_global_ptr() {
; CHECK: Printing analysis 'Escape Analysis' for function 'assigning_global_ptr':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  store i32 20, ptr %x, align 4
  store ptr %x, ptr @GPtr, align 8
  ret void
}

define dso_local ptr @escape_local() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_local':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-NEXT:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  ret ptr %x
}

define dso_local ptr @escape_by_returning_ptr() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_by_returning_ptr':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %x = alloca ptr, align 8
entry:
  %x = alloca ptr, align 8
  %0 = load ptr, ptr %x, align 8
  ret ptr %0
}

; void ptrtoint_and_inttoptr() {
; 	int y;
; 	int *x = (int *)(long int)&y;
; 	GPtr = x;
; }
define void @ptrtoint_and_inttoptr() #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'ptrtoint_and_inttoptr':
; CHECK-NEXT: Escaping objects for BB entry:
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

; Pointer escapes itself when passing it to external function
; Escaping local pointer
; void escaping_ptr() {
;   int *x;
;   external_func(x);
;   *x = 777;
; }
define dso_local void @escaping_ptr() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escaping_ptr':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-NEXT:   %x = alloca ptr, align 8
entry:
  %x = alloca ptr, align 8
  %0 = load ptr, ptr %x, align 8
  call void @external_func(ptr noundef %0)
  %1 = load ptr, ptr %x, align 8
  store i32 777, ptr %1, align 4
  ret void
}
