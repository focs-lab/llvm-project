; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

; C code:

; CHECK: Printing analysis 'Escape Analysis' for module '<stdin>':

@.str = private unnamed_addr constant [3 x i8] c"%p\00", align 1
@GPtr = global ptr null, align 8

define dso_local void @caller() {
; CHECK: Printing analysis 'Escape Analysis' for function 'caller':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  call void @func_with_ptr_arg(ptr noundef %x, ptr noundef %y)
  ret void
}

define internal void @func_with_ptr_arg(ptr noundef %x, ptr noundef %y) {
; CHECK: Printing analysis 'Escape Analysis' for function 'func_with_ptr_arg':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %x
entry:
  %x.addr = alloca ptr, align 8
  %y.addr = alloca ptr, align 8
  %Alias = alloca ptr, align 8
  store ptr %x, ptr %x.addr, align 8
  store ptr %y, ptr %y.addr, align 8
  %0 = load ptr, ptr %x.addr, align 8
  store ptr %0, ptr %Alias, align 8
  %call = call i32 @rand() #2
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.end

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG: ptr %x
if.then:                                          ; preds = %entry
  %1 = load ptr, ptr %Alias, align 8
  store ptr %1, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG: ptr %x
; CHECK-DAG: ptr %y
if.end:                                           ; preds = %if.then, %entry
  %2 = load ptr, ptr %y.addr, align 8
  store ptr %2, ptr @GPtr, align 8
  ret void
}

declare i32 @printf(ptr noundef, ...)
declare i32 @rand() #1
