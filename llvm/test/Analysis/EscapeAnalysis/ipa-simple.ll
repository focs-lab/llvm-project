; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

; C code:
; void level2_func1(int *x) {
; 	GPtr = x;											// Escape through GPtr
; }
;
; void level2_func2(int *x) {
; 	*x = 333;
; }
;
; void level1_func1(int *x) {
; 	level2_func1(x);
; }
;
; void level1_func2(int *x) {
; 	*x = 333;
; 	int *p = x;
; 	printf("%p\n", &p);			// Escape through external func call
; }
;
; void level1_func3(int *x, int *p) {
; 	*x = 42;
; 	level2_func2(x);
; 	level2_func1(p);
; }
;
; void external_func(int *x);
;
; void parent_func1() {
; 	int x;
; 	level1_func1(&x);
; 	level1_func2(&x);
; 	int y;
; 	external_func(&y);
; 	int z, p;
; 	level1_func3(&z, &p);
; }

; CHECK: Printing analysis 'Escape Analysis' for module '<stdin>':

@GPtr = dso_local global ptr null, align 8
@.str = private unnamed_addr constant [4 x i8] c"%p\0A\00", align 1

define dso_local void @level2_func1(ptr noundef %x) #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'level2_func1':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %x
entry:
  store ptr %x, ptr @GPtr, align 8
  ret void
}

define dso_local void @level2_func2(ptr noundef %x) #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'level2_func2':
; CHECK-NOT: Escaping objects for BB entry:
entry:
  store i32 333, ptr %x, align 4
  ret void
}

define dso_local void @level1_func1(ptr noundef %x) #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'level1_func1':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %x
entry:
  call void @level2_func1(ptr noundef %x)
  ret void
}

define dso_local void @level1_func2(ptr noundef %x) {
; CHECK: Printing analysis 'Escape Analysis' for function 'level1_func2':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %p = alloca ptr, align 8
; CHECK-DAG: ptr %x
entry:
  %p = alloca ptr, align 8
  store i32 333, ptr %x, align 4
  store ptr %x, ptr %p, align 8
  %call = call i32 (ptr, ...) @printf(ptr noundef @.str, ptr noundef %p)
  ret void
}

declare i32 @printf(ptr noundef, ...)

define dso_local void @level1_func3(ptr noundef %x, ptr noundef %p) {
; CHECK: Printing analysis 'Escape Analysis' for function 'level1_func3':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %p
entry:
  store i32 42, ptr %x, align 4
  call void @level2_func2(ptr noundef %x)
  call void @level2_func1(ptr noundef %p)
  ret void
}

define dso_local void @parent_func1() {
; CHECK: Printing analysis 'Escape Analysis' for function 'parent_func1':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK-DAG:   %p = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  %z = alloca i32, align 4
  %p = alloca i32, align 4
  call void @level1_func1(ptr noundef %x)
  call void @level1_func2(ptr noundef %x)
  call void @external_func(ptr noundef %y)
  call void @level1_func3(ptr noundef %z, ptr noundef %p)
  ret void
}

declare void @external_func(ptr noundef)