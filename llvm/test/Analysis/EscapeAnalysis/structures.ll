; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8
%struct.StructTy = type { ptr }
%struct.StructWithoutPointers = type { i32, i32 }

define dso_local ptr @escape_struct_field() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_struct_field':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %s = alloca ptr, align 8
; CHECK-DAG:   %x = alloca i32, align 4
entry:
  %s = alloca ptr, align 8
  %x = alloca i32, align 4
  %0 = load ptr, ptr %s, align 8
  %field = getelementptr inbounds %struct.StructTy, ptr %0, i32 0, i32 0
  store ptr %x, ptr %field, align 8
  %1 = load ptr, ptr %s, align 8
  ret ptr %1
}

define dso_local void @escape_memcpy() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_memcpy':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:  %S2 = alloca %struct.StructTy, align 8
; CHECK-DAG:  %S1 = alloca %struct.StructTy, align 8
entry:
  %S1 = alloca %struct.StructTy, align 8
  %S2 = alloca %struct.StructTy, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %S2, ptr align 8 %S1, i64 8, i1 false)
  %field = getelementptr inbounds %struct.StructTy, ptr %S2, i32 0, i32 0
  store ptr %field, ptr @GPtr, align 8
  ret void
}

define dso_local void @no_escape_memcpy() {
; CHECK: Printing analysis 'Escape Analysis' for function 'no_escape_memcpy':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG: %S2 = alloca %struct.StructWithoutPointers, align 4
; CHECK-NOT: %S1 = alloca %struct.StructWithoutPointers, align 4
entry:
  %S1 = alloca %struct.StructWithoutPointers, align 4
  %S2 = alloca %struct.StructWithoutPointers, align 4
  call void @llvm.memcpy.p0.p0.i64(ptr align 4 %S2, ptr align 4 %S1, i64 8, i1 false)
  %Field1 = getelementptr inbounds %struct.StructWithoutPointers, ptr %S2, i32 0, i32 0
  store ptr %Field1, ptr @GPtr, align 8
  ret void
}

%struct.InnerStructTy = type { ptr }
%struct.OuterStructTy = type { %struct.InnerStructTy }
@GS = dso_local global ptr null, align 8

define dso_local void @escape_nested_struct() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_nested_struct':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK-DAG:   %OuterS = alloca %struct.OuterStructTy, align 8
; CHECK-DAG:   %InnerS = alloca %struct.InnerStructTy, align 8
entry:
  %x = alloca i32, align 4
  %InnerS = alloca %struct.InnerStructTy, align 8
  %OuterS = alloca %struct.OuterStructTy, align 8
  store i32 333, ptr %x, align 4
  %Ptr = getelementptr inbounds %struct.InnerStructTy, ptr %InnerS, i32 0, i32 0
  store ptr %x, ptr %Ptr, align 8
  %Inner = getelementptr inbounds %struct.OuterStructTy, ptr %OuterS, i32 0, i32 0
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %Inner, ptr align 8 %InnerS, i64 8, i1 false)
  store ptr %OuterS, ptr @GS, align 8
  ret void
}