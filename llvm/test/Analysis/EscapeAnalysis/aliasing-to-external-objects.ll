; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

%struct.StructTy = type { ptr }

@GPtr = dso_local global ptr null, align 8

define void @escape_through_ptr_argument_aliasing(ptr noundef %k) #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_through_ptr_argument_aliasing':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %p = alloca ptr, align 8
; CHECK-DAG:  %k.addr = alloca ptr, align 8
entry:
  %k.addr = alloca ptr, align 8
  %p = alloca ptr, align 8
  store ptr %k, ptr %k.addr, align 8
  %0 = load ptr, ptr %k.addr, align 8
  store ptr %0, ptr %p, align 8
  ret void
}

define void @escape_through_gptr_aliasing() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_through_gptr_aliasing':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %GPtrAlias = alloca ptr, align 8
entry:
  %GPtrAlias = alloca ptr, align 8
  %0 = load ptr, ptr @GPtr, align 8
  store ptr %0, ptr %GPtrAlias, align 8
  ret void
}