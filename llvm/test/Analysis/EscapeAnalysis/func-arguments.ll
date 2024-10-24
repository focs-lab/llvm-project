; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8

; Function Attrs: noinline nounwind uwtable
define dso_local void @external_func(ptr noundef %ptr) #0 {
entry:
  ret void
}

define dso_local void @escaping_func_arguments(i32 noundef %x, i32 noundef %y, ptr noundef %z) {
; CHECK: Printing analysis 'Escape Analysis' for function 'escaping_func_arguments':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG: i32 %x
; CHECK-DAG: i32 %y
; CHECK-DAG:   %y.addr = alloca i32, align 4
; CHECK-DAG:   %x.addr = alloca i32, align 4
entry:
  %x.addr = alloca i32, align 4
  %y.addr = alloca i32, align 4
  store i32 %x, ptr %x.addr, align 4
  store i32 %y, ptr %y.addr, align 4
  store ptr %x.addr, ptr @GPtr, align 8
  call void @external_func(ptr noundef %y.addr)
  ret void
}