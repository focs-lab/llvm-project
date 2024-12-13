; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8
@.str = private unnamed_addr constant [3 x i8] c"%p\00", align 1
@.str.1 = private unnamed_addr constant [4 x i8] c"%p\0A\00", align 1

define dso_local void @func1(ptr noundef %x, ptr noundef %y, ptr noundef %z, ptr noundef %NoEsc) {
; CHECK: Printing analysis 'Escape Analysis' for function 'func1':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %x.addr = alloca ptr, align 8
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %z.addr = alloca ptr, align 8
; CHECK-DAG: ptr %y
; CHECK-DAG: ptr %x
; CHECK-DAG: ptr %z
; CHECK-DAG:   %Alias = alloca ptr, align 8
entry:
  %x.addr = alloca ptr, align 8
  %y.addr = alloca ptr, align 8
  %z.addr = alloca ptr, align 8
  %NoEsc.addr = alloca ptr, align 8
  %Alias = alloca ptr, align 8
  store ptr %x, ptr %x.addr, align 8
  store ptr %y, ptr %y.addr, align 8
  store ptr %z, ptr %z.addr, align 8
  store ptr %NoEsc, ptr %NoEsc.addr, align 8
  %0 = load ptr, ptr %x.addr, align 8
  store ptr %0, ptr %Alias, align 8
  %1 = load ptr, ptr %Alias, align 8
  store i32 333, ptr %1, align 4
  %call = call i32 @rand() #3
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.end

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG:   %x.addr = alloca ptr, align 8
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %z.addr = alloca ptr, align 8
; CHECK-DAG: ptr %y
; CHECK-DAG: ptr %x
; CHECK-DAG: ptr %z
; CHECK-DAG:   %Alias = alloca ptr, align 8
if.then:                                          ; preds = %entry
  %2 = load ptr, ptr %x.addr, align 8
  store ptr %2, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:   %x.addr = alloca ptr, align 8
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %z.addr = alloca ptr, align 8
; CHECK-DAG: ptr %y
; CHECK-DAG: ptr %x
; CHECK-DAG: ptr %z
; CHECK-DAG:   %Alias = alloca ptr, align 8
if.end:                                           ; preds = %if.then, %entry
  %3 = load ptr, ptr %y.addr, align 8
  %call1 = call i32 (ptr, ...) @printf(ptr noundef @.str, ptr noundef %3)
  %4 = load ptr, ptr %x.addr, align 8
  %5 = load ptr, ptr %y.addr, align 8
  %6 = load ptr, ptr %z.addr, align 8
  call void @func2(ptr noundef %4, ptr noundef %5, ptr noundef %6)
  ret void
}

declare i32 @rand()
declare i32 @printf(ptr noundef, ...)

define dso_local void @func2(ptr noundef %x, ptr noundef %y, ptr noundef %z) {
; CHECK: Printing analysis 'Escape Analysis' for function 'func2':
; CHECK: Escaping objects for BB entry:
; CHECK-DAG:   %x.addr = alloca ptr, align 8
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %z.addr = alloca ptr, align 8
; CHECK-DAG: ptr %y
; CHECK-DAG: ptr %x
; CHECK-DAG: ptr %z
entry:
  %x.addr = alloca ptr, align 8
  %y.addr = alloca ptr, align 8
  %z.addr = alloca ptr, align 8
  %NoEsc = alloca ptr, align 8
  store ptr %x, ptr %x.addr, align 8
  store ptr %y, ptr %y.addr, align 8
  store ptr %z, ptr %z.addr, align 8
  %call = call i32 @rand() #3
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.end

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG:   %x.addr = alloca ptr, align 8
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %z.addr = alloca ptr, align 8
; CHECK-DAG: ptr %y
; CHECK-DAG: ptr %x
; CHECK-DAG: ptr %z
if.then:
  call void @func1(ptr noundef %x.addr, ptr noundef %y.addr, ptr noundef %z.addr, ptr noundef %NoEsc)
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:   %x.addr = alloca ptr, align 8
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %z.addr = alloca ptr, align 8
; CHECK-DAG: ptr %y
; CHECK-DAG: ptr %x
; CHECK-DAG: ptr %z
if.end:
  %0 = load ptr, ptr %z.addr, align 8
  %call1 = call i32 (ptr, ...) @printf(ptr noundef @.str.1, ptr noundef %0)
  ret void
}

define dso_local void @caller() {
; CHECK: Printing analysis 'Escape Analysis' for function 'caller':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %z = alloca i32, align 4
; CHECK-DAG:  %y = alloca i32, align 4
; CHECK-DAG:  %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  %z = alloca i32, align 4
  %NoEsc = alloca i32, align 4
  call void @func1(ptr noundef %x, ptr noundef %y, ptr noundef %z, ptr noundef %NoEsc)
  ret void
}