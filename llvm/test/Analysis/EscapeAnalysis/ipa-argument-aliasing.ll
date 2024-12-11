; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

@.str = private unnamed_addr constant [4 x i8] c"%p\0A\00", align 1
@GPtr = global ptr null, align 8

; CHECK: Printing analysis 'Escape Analysis' for module '<stdin>':

; C code:
; void alias_of_ptr_arg(int *PtrArg) {
; 	int *Alias = PtrArg;
; 	printf("%p\n", PtrArg);
; }

define void @alias_of_ptr_arg(ptr noundef %PtrArg) {
; CHECK: Printing analysis 'Escape Analysis' for function 'alias_of_ptr_arg':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %PtrArg.addr = alloca ptr, align 8
; CHECK-DAG: ptr %PtrArg
; CHECK-DAG:  %Alias = alloca ptr, align 8
entry:
  %PtrArg.addr = alloca ptr, align 8
  %Alias = alloca ptr, align 8
  store ptr %PtrArg, ptr %PtrArg.addr, align 8
  %0 = load ptr, ptr %PtrArg.addr, align 8
  store ptr %0, ptr %Alias, align 8
  %1 = load ptr, ptr %PtrArg.addr, align 8
  %call = call i32 (ptr, ...) @printf(ptr noundef @.str, ptr noundef %1)
  ret void
}

; C code:
; void alias_of_ptr_arg_2(int *PtrArg1, int *PtrArg2) {
; 	int *x = PtrArg1;
; 	int *y = PtrArg2;
; 	if (rand())
; 		GPtr = x;
; 	else
; 		GPtr = y;
; }
define void @alias_of_ptr_arg_2(ptr noundef %PtrArg1, ptr noundef %PtrArg2) {
; CHECK: Printing analysis 'Escape Analysis' for function 'alias_of_ptr_arg_2':
; CHECK-NOT: Escaping objects for BB entry:
entry:
  %PtrArg1.addr = alloca ptr, align 8
  %PtrArg2.addr = alloca ptr, align 8
  %x = alloca ptr, align 8
  %y = alloca ptr, align 8
  store ptr %PtrArg1, ptr %PtrArg1.addr, align 8
  store ptr %PtrArg2, ptr %PtrArg2.addr, align 8
  %0 = load ptr, ptr %PtrArg1.addr, align 8
  store ptr %0, ptr %x, align 8
  %1 = load ptr, ptr %PtrArg2.addr, align 8
  store ptr %1, ptr %y, align 8
  %call = call i32 @rand()
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.else

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG: ptr %PtrArg1
; CHECK-DAG:   %x = alloca ptr, align 8
; CHECK-DAG:   %PtrArg1.addr = alloca ptr, align 8
if.then:                                          ; preds = %entry
  %2 = load ptr, ptr %x, align 8
  store ptr %2, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.else:
; CHECK-DAG:  %y = alloca ptr, align 8
; CHECK-DAG:ptr %PtrArg2
; CHECK-DAG:  %PtrArg2.addr = alloca ptr, align 8
if.else:                                          ; preds = %entry
  %3 = load ptr, ptr %y, align 8
  store ptr %3, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:   %y = alloca ptr, align 8
; CHECK-DAG: ptr %PtrArg1
; CHECK-DAG: ptr %PtrArg2
; CHECK-DAG:   %x = alloca ptr, align 8
; CHECK-DAG:   %PtrArg2.addr = alloca ptr, align 8
; CHECK-DAG:   %PtrArg1.addr = alloca ptr, align 8
if.end:                                           ; preds = %if.else, %if.then
  ret void
}

declare i32 @printf(ptr noundef, ...)
declare i32 @rand()
