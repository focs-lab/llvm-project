; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

%struct.StructTy = type { ptr }

@GPtr = dso_local global ptr null, align 8

define dso_local ptr @escape_func() #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_func':
; CHECK-NEXT: Escaping variables:
; CHECK-NEXT:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  store i32 30, ptr %x, align 4
  call void @external_func(ptr noundef %x)
  ret ptr %x
}

define dso_local void @external_func(ptr noundef %ptr) #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'external_func':
; CHECK-NOT: Escaping variables:
  ret void
}

define dso_local void @assigning_global_ptr() {
; CHECK: Printing analysis 'Escape Analysis' for function 'assigning_global_ptr':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  store i32 20, ptr %x, align 4
  store ptr %x, ptr @GPtr, align 8
  ret void
}
