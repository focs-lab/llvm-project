; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

; C code:

; CHECK: Printing analysis 'Escape Analysis' for module '<stdin>':

@.str = private unnamed_addr constant [3 x i8] c"%p\00", align 1
@GPtr = global ptr null, align 8

; C code:
; void use_esc_func(int *x, int *y) {
; 	// GPtr is escaping, but it's obvious, not print
; 	printf("%p", GPtr);
; 	// GPtr is escaping, but it's obvious, not print
; 	GPtr = GPtr;
; 	// x escapes here
; 	printf("%p", x);
; 	// Alias escapes as an alias of GPtr
; 	int *Alias = GPtr + 3;
; 	// xAlias escapes as x escapes
; 	int *xAlias = x + 3;
; 	// yAlias doesn't escape as y doesn't escaps
; 	int *yAlias = y + 3;
; }

; CHECK: Printing analysis 'Escape Analysis' for function 'use_esc_func':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %x.addr = alloca ptr, align 8
; CHECK-DAG:  %xAlias = alloca ptr, align 8
; CHECK-DAG:  %Alias = alloca ptr, align 8
; CHECK-DAG: ptr %x
define void @use_esc_func(ptr noundef %x, ptr noundef %y) {
entry:
  %x.addr = alloca ptr, align 8
  %y.addr = alloca ptr, align 8
  %Alias = alloca ptr, align 8
  %xAlias = alloca ptr, align 8
  %yAlias = alloca ptr, align 8
  store ptr %x, ptr %x.addr, align 8
  store ptr %y, ptr %y.addr, align 8
  %0 = load ptr, ptr @GPtr, align 8
  %call = call i32 (ptr, ...) @printf(ptr noundef @.str, ptr noundef %0)
  %1 = load ptr, ptr @GPtr, align 8
  store ptr %1, ptr @GPtr, align 8
  %2 = load ptr, ptr %x.addr, align 8
  %call1 = call i32 (ptr, ...) @printf(ptr noundef @.str, ptr noundef %2)
  %3 = load ptr, ptr @GPtr, align 8
  %add.ptr = getelementptr inbounds i32, ptr %3, i64 3
  store ptr %add.ptr, ptr %Alias, align 8
  %4 = load ptr, ptr %x.addr, align 8
  %add.ptr2 = getelementptr inbounds i32, ptr %4, i64 3
  store ptr %add.ptr2, ptr %xAlias, align 8
  %5 = load ptr, ptr %y.addr, align 8
  %add.ptr3 = getelementptr inbounds i32, ptr %5, i64 3
  store ptr %add.ptr3, ptr %yAlias, align 8
  ret void
}

; void *func_with_ptr_arg(int *x, int *y) {
; 	int *Alias = x;
; 	*Alias = 333;
; 	if (rand())
; 		GPtr = x;
; 	GPtr = y;
; 	return NULL;
; }

define dso_local ptr @func_with_ptr_arg(ptr noundef %x, ptr noundef %y) {
; CHECK: Printing analysis 'Escape Analysis' for function 'func_with_ptr_arg':
; CHECK-NOT: Escaping objects for BB entry:
entry:
  %x.addr = alloca ptr, align 8
  %y.addr = alloca ptr, align 8
  %Alias = alloca ptr, align 8
  store ptr %x, ptr %x.addr, align 8
  store ptr %y, ptr %y.addr, align 8
  %0 = load ptr, ptr %x.addr, align 8
  store ptr %0, ptr %Alias, align 8
  %1 = load ptr, ptr %Alias, align 8
  store i32 333, ptr %1, align 4
  %call = call i32 @rand() #2
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.end

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG:   %Alias = alloca ptr, align 8
; CHECK-DAG: ptr %x
; CHECK-DAG:   %x.addr = alloca ptr, align 8
if.then:                                          ; preds = %entry
  %2 = load ptr, ptr %x.addr, align 8
  store ptr %2, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:   %Alias = alloca ptr, align 8
; CHECK-DAG: ptr %x
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %x.addr = alloca ptr, align 8
; CHECK-DAG: ptr %y
if.end:                                           ; preds = %if.then, %entry
  %3 = load ptr, ptr %y.addr, align 8
  store ptr %3, ptr @GPtr, align 8
  ret ptr null
}

declare i32 @printf(ptr noundef, ...)
declare i32 @rand() #1
