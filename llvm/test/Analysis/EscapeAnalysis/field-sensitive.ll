; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

%struct.SimpleStructTy = type { i32, i32, i32 }
%struct.GrandParentStructTy = type { i32, i32, i32, %struct.ParentStructTy, %struct.SimpleStructTy }
%struct.ParentStructTy = type { i32, i32, i32, %struct.SimpleStructTy }

@GPtr = dso_local global ptr null, align 8
@GV = dso_local global i32 0, align 4
@GPtrPtr = dso_local global ptr null, align 8

; CHECK: Printing analysis 'Escape Analysis' for function 'field_sensitive_simple_struct':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %S = alloca %struct.SimpleStructTy, align 4 | Path: 1
; CHECK-DAG:   %S = alloca %struct.SimpleStructTy, align 4 | Path: 2
define dso_local void @field_sensitive_simple_struct() {
entry:
  %S = alloca %struct.SimpleStructTy, align 4
  %y = getelementptr inbounds %struct.SimpleStructTy, ptr %S, i32 0, i32 1
  store ptr %y, ptr @GPtr, align 8
  %z = getelementptr inbounds %struct.SimpleStructTy, ptr %S, i32 0, i32 2
  store ptr %z, ptr @GPtr, align 8
  ret void
}

; CHECK: Printing analysis 'Escape Analysis' for function 'escape_through_escaped_pointer':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %S = alloca %struct.SimpleStructTy, align 4 | Path: 2
; CHECK-DAG:   %SPtr = alloca ptr, align 8 | Path: 2
define dso_local void @escape_through_escaped_pointer() {
entry:
  %S = alloca %struct.SimpleStructTy, align 4
  %SPtr = alloca ptr, align 8
  %SPtrPtr = alloca ptr, align 8
  store ptr %S, ptr %SPtr, align 8
  store ptr %SPtr, ptr %SPtrPtr, align 8
  %0 = load ptr, ptr %SPtrPtr, align 8
  %1 = load ptr, ptr %0, align 8
  %z = getelementptr inbounds %struct.SimpleStructTy, ptr %1, i32 0, i32 2
  store ptr %z, ptr @GPtr, align 8
  ret void
}

; CHECK: Printing analysis 'Escape Analysis' for function 'field_sensitive_simple_struct_ptr':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %S = alloca %struct.SimpleStructTy, align 4 | Path: 1
; CHECK-DAG:   %S = alloca %struct.SimpleStructTy, align 4 | Path: 2
define dso_local void @field_sensitive_simple_struct_ptr() {
entry:
  %S = alloca %struct.SimpleStructTy, align 4
  %SPtr = alloca ptr, align 8
  store ptr %S, ptr %SPtr, align 8
  %0 = load ptr, ptr %SPtr, align 8
  %y = getelementptr inbounds %struct.SimpleStructTy, ptr %0, i32 0, i32 1
  store ptr %y, ptr @GPtr, align 8
  %1 = load ptr, ptr %SPtr, align 8
  %z = getelementptr inbounds %struct.SimpleStructTy, ptr %1, i32 0, i32 2
  store ptr %z, ptr @GPtr, align 8
  ret void
}

; CHECK: Printing analysis 'Escape Analysis' for function 'field_sensitive_nested_struct':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %GPS = alloca %struct.GrandParentStructTy, align 4 | Path: 1 3 3
; CHECK-DAG:   %GPS = alloca %struct.GrandParentStructTy, align 4 | Path: 1 4
; CHECK-DAG:   %GPS = alloca %struct.GrandParentStructTy, align 4 | Path: 2 3 3
; CHECK-DAG:   %GPS = alloca %struct.GrandParentStructTy, align 4 | Path: 2 4
define dso_local void @field_sensitive_nested_struct() {
entry:
  %GPS = alloca %struct.GrandParentStructTy, align 4
  %P = getelementptr inbounds %struct.GrandParentStructTy, ptr %GPS, i32 0, i32 3
  %S = getelementptr inbounds %struct.ParentStructTy, ptr %P, i32 0, i32 3
  %y = getelementptr inbounds %struct.SimpleStructTy, ptr %S, i32 0, i32 1
  store ptr %y, ptr @GPtr, align 8
  %P1 = getelementptr inbounds %struct.GrandParentStructTy, ptr %GPS, i32 0, i32 3
  %S2 = getelementptr inbounds %struct.ParentStructTy, ptr %P1, i32 0, i32 3
  %z = getelementptr inbounds %struct.SimpleStructTy, ptr %S2, i32 0, i32 2
  store ptr %z, ptr @GPtr, align 8
  %S3 = getelementptr inbounds %struct.GrandParentStructTy, ptr %GPS, i32 0, i32 4
  %y4 = getelementptr inbounds %struct.SimpleStructTy, ptr %S3, i32 0, i32 1
  store ptr %y4, ptr @GPtr, align 8
  %S5 = getelementptr inbounds %struct.GrandParentStructTy, ptr %GPS, i32 0, i32 4
  %z6 = getelementptr inbounds %struct.SimpleStructTy, ptr %S5, i32 0, i32 2
  store ptr %z6, ptr @GPtr, align 8
  ret void
}

; CHECK: Printing analysis 'Escape Analysis' for function 'field_sensitive_nested_struct_ptr':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %GPS = alloca %struct.GrandParentStructTy, align 4 | Path: 1 3 3
; CHECK-DAG:   %GPS = alloca %struct.GrandParentStructTy, align 4 | Path: 1 4
; CHECK-DAG:   %GPS = alloca %struct.GrandParentStructTy, align 4 | Path: 2 3 3
; CHECK-DAG:   %GPS = alloca %struct.GrandParentStructTy, align 4 | Path: 2 4
define dso_local void @field_sensitive_nested_struct_ptr() {
entry:
  %GPS = alloca %struct.GrandParentStructTy, align 4
  %GPSPtr = alloca ptr, align 8
  store ptr %GPS, ptr %GPSPtr, align 8
  %0 = load ptr, ptr %GPSPtr, align 8
  %P = getelementptr inbounds %struct.GrandParentStructTy, ptr %0, i32 0, i32 3
  %S = getelementptr inbounds %struct.ParentStructTy, ptr %P, i32 0, i32 3
  %y = getelementptr inbounds %struct.SimpleStructTy, ptr %S, i32 0, i32 1
  store ptr %y, ptr @GPtr, align 8
  %1 = load ptr, ptr %GPSPtr, align 8
  %P1 = getelementptr inbounds %struct.GrandParentStructTy, ptr %1, i32 0, i32 3
  %S2 = getelementptr inbounds %struct.ParentStructTy, ptr %P1, i32 0, i32 3
  %z = getelementptr inbounds %struct.SimpleStructTy, ptr %S2, i32 0, i32 2
  store ptr %z, ptr @GPtr, align 8
  %2 = load ptr, ptr %GPSPtr, align 8
  %S3 = getelementptr inbounds %struct.GrandParentStructTy, ptr %2, i32 0, i32 4
  %y4 = getelementptr inbounds %struct.SimpleStructTy, ptr %S3, i32 0, i32 1
  store ptr %y4, ptr @GPtr, align 8
  %3 = load ptr, ptr %GPSPtr, align 8
  %S5 = getelementptr inbounds %struct.GrandParentStructTy, ptr %3, i32 0, i32 4
  %z6 = getelementptr inbounds %struct.SimpleStructTy, ptr %S5, i32 0, i32 2
  store ptr %z6, ptr @GPtr, align 8
  ret void
}