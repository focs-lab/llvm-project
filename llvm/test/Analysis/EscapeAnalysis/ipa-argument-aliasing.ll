; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

@.str = private unnamed_addr constant [4 x i8] c"%p\0A\00", align 1
@GPtr = global ptr null, align 8

; CHECK: Printing analysis 'Escape Analysis' for module '<stdin>':

define dso_local void @alias_of_ptr_arg_2(ptr noundef %Arg1, ptr noundef %Arg2) #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'alias_of_ptr_arg_2':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %Arg2
; CHECK-DAG: ptr %Arg1
; CHECK-DAG:   %Arg1.addr = alloca ptr, align 8
; CHECK-DAG:   %Arg2.addr = alloca ptr, align 8
; CHECK-DAG:   %x = alloca ptr, align 8
entry:
  %Arg1.addr = alloca ptr, align 8
  %Arg2.addr = alloca ptr, align 8
  %x = alloca ptr, align 8
  %y = alloca ptr, align 8
  store ptr %Arg1, ptr %Arg1.addr, align 8
  store ptr %Arg2, ptr %Arg2.addr, align 8
  %0 = load ptr, ptr %Arg1.addr, align 8
  store ptr %0, ptr %x, align 8
  %call = call i32 @rand() #2
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.else

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG: ptr %Arg2
; CHECK-DAG: ptr %Arg1
; CHECK-DAG:   %Arg1.addr = alloca ptr, align 8
; CHECK-DAG:   %Arg2.addr = alloca ptr, align 8
; CHECK-DAG:   %x = alloca ptr, align 8
if.then:
  %1 = load ptr, ptr %x, align 8
  store ptr %1, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.else:
; CHECK-DAG: ptr %Arg2
; CHECK-DAG: ptr %Arg1
; CHECK-DAG:   %Arg1.addr = alloca ptr, align 8
; CHECK-DAG:   %Arg2.addr = alloca ptr, align 8
; CHECK-DAG:   %x = alloca ptr, align 8
; CHECK-DAG:   %y = alloca ptr, align 8
if.else:
  %2 = load ptr, ptr %Arg2.addr, align 8
  store ptr %2, ptr %y, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG: ptr %Arg2
; CHECK-DAG: ptr %Arg1
; CHECK-DAG:   %Arg1.addr = alloca ptr, align 8
; CHECK-DAG:   %Arg2.addr = alloca ptr, align 8
; CHECK-DAG:   %x = alloca ptr, align 8
; CHECK-DAG:   %y = alloca ptr, align 8
if.end:
  ret void
}

; CHECK: Printing analysis 'Escape Analysis' for function 'alias_of_ptr_arg_2_caller':
; CHECK-NEXT:  Escaping objects for BB entry:
; CHECK-DAG:    %x = alloca i32, align 4
define dso_local void @alias_of_ptr_arg_2_caller() {
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  call void @alias_of_ptr_arg_2(ptr noundef %x, ptr noundef %y)
  ret void
}

declare i32 @printf(ptr noundef, ...)
declare i32 @rand()
