; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

; C code:
; static void level2_func1(int *x) {
;   GPtr = x; // Escape through GPtr
; }
;
; static void level2_func2(int *x) { *x = 333; }
;
; static void level1_func1(int *x) { level2_func1(x); }
;
; static void level1_func2(int *x) {
;   *x = 333;
;   int *p = x;
;   printf("%p\n", &p); // Escape through external func call
; }
;
; struct StructTy { int *x; };
;
; static void level1_func3(int *x, int *y, int *z) {
;   *x = 42;
;   level2_func2(x);  // x doesn't escape level2_func2
;   level2_func2(y);  // p doesn't escape in level2_func2
;   level2_func1(y);  // p escapes in level2_func1
;
;   struct StructTy S1;
;   // S1 doesn't escape, because x doesn't escape
;   S1.x = x;
;
;   struct StructTy S2;
;   // S2 escapes because x escapes (bottom-top)
;   S2.x = y;
;
;   struct StructTy S3;
;   // S3 escapes because z escapes (from top-bottom)
;   S3.x = z;
; }
;
; void external_func(int *x);
;
; void parent_func() {
;   int x;
;   level1_func1(&x);
;   level1_func2(&x);
;
;   int y;
;   external_func(&y);
;
;   int p, q;
;   level1_func3(&p, &q, &y);
; }

%struct.StructTy = type { ptr }

@GPtr = dso_local global ptr null, align 8
@.str = private unnamed_addr constant [4 x i8] c"%p\0A\00", align 1

define dso_local void @parent_func() {
; CHECK: Printing analysis 'Escape Analysis' for function 'parent_func':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %q = alloca i32, align 4
; CHECK-DAG:  %y = alloca i32, align 4
; CHECK-DAG:  %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  %p = alloca i32, align 4
  %q = alloca i32, align 4
  call void @level1_func1(ptr noundef %x)
  call void @level1_func2(ptr noundef %x)
  call void @external_func(ptr noundef %y)
  call void @level1_func3(ptr noundef %p, ptr noundef %q, ptr noundef %y)
  ret void
}

define internal void @level1_func1(ptr noundef %x) {
; CHECK: Printing analysis 'Escape Analysis' for function 'level1_func1':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:ptr %x
entry:
  call void @level2_func1(ptr noundef %x)
  ret void
}

define internal void @level1_func2(ptr noundef %x) {
; CHECK: Printing analysis 'Escape Analysis' for function 'level1_func2':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:ptr %x
; CHECK-DAG:  %p = alloca ptr, align 8
entry:
  %p = alloca ptr, align 8
  store i32 333, ptr %x, align 4
  store ptr %x, ptr %p, align 8
  %call = call i32 (ptr, ...) @printf(ptr noundef @.str, ptr noundef %p)
  ret void
}

declare void @external_func(ptr noundef) #1

define internal void @level1_func3(ptr noundef %x, ptr noundef %y, ptr noundef %z) {
; CHECK: Printing analysis 'Escape Analysis' for function 'level1_func3':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:ptr %y
; CHECK-DAG:ptr %z
; CHECK-DAG:  %S2 = alloca %struct.StructTy, align 8
; CHECK-DAG:  %S3 = alloca %struct.StructTy, align 8
entry:
  %S1 = alloca %struct.StructTy, align 8
  %S2 = alloca %struct.StructTy, align 8
  %S3 = alloca %struct.StructTy, align 8
  store i32 42, ptr %x, align 4
  call void @level2_func2(ptr noundef %x)
  call void @level2_func2(ptr noundef %y)
  call void @level2_func1(ptr noundef %y)
  %x1 = getelementptr inbounds %struct.StructTy, ptr %S1, i32 0, i32 0
  store ptr %x, ptr %x1, align 8
  %x2 = getelementptr inbounds %struct.StructTy, ptr %S2, i32 0, i32 0
  store ptr %y, ptr %x2, align 8
  %x3 = getelementptr inbounds %struct.StructTy, ptr %S3, i32 0, i32 0
  store ptr %z, ptr %x3, align 8
  ret void
}

define internal void @level2_func2(ptr noundef %x) {
; CHECK: Printing analysis 'Escape Analysis' for function 'level2_func2':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:ptr %x
entry:
  store i32 333, ptr %x, align 4
  ret void
}

define internal void @level2_func1(ptr noundef %x) {
; CHECK: Printing analysis 'Escape Analysis' for function 'level2_func1':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:ptr %x
entry:
  store ptr %x, ptr @GPtr, align 8
  ret void
}

declare i32 @printf(ptr noundef, ...)