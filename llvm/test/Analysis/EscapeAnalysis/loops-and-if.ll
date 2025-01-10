; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8

define dso_local void @escape_in_conditional() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_in_conditional':
; CHECK-NOT: Escaping objects for BB entry:
; CHECK: Escaping objects for BB if.then:
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  %0 = load ptr, ptr @GPtr, align 8
  %cmp = icmp eq ptr %0, null
  br i1 %cmp, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  store ptr %x, ptr @GPtr, align 8
  br label %if.end

if.end:                                           ; preds = %if.then, %entry
  ret void
}

define dso_local void @escape_in_loop() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_in_loop':
; CHECK: Escaping objects for BB for.cond:
; CHECK-DAG:   %i = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK: Escaping objects for BB for.body:
; CHECK-DAG:   %i = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK: Escaping objects for BB if.then:
; CHECK-DAG:   %i = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK: Escaping objects for BB if.else:
; CHECK-DAG:   %i = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:   %i = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK: Escaping objects for BB for.inc:
; CHECK-DAG:   %i = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK: Escaping objects for BB for.end:
; CHECK-DAG:   %i = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
entry:
  %i = alloca i32, align 4
  %x = alloca i32, align 4
  store i32 0, ptr %i, align 4
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %0 = load i32, ptr %i, align 4
  %cmp = icmp slt i32 %0, 10
  br i1 %cmp, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  %1 = load i32, ptr %i, align 4
  %cmp1 = icmp eq i32 %1, 5
  br i1 %cmp1, label %if.then, label %if.else

if.then:                                          ; preds = %for.body
  store ptr %x, ptr @GPtr, align 8
  br label %if.end

if.else:                                          ; preds = %for.body
  store ptr %i, ptr @GPtr, align 8
  br label %if.end

if.end:                                           ; preds = %if.else, %if.then
  br label %for.inc

for.inc:                                          ; preds = %if.end
  %2 = load i32, ptr %i, align 4
  %inc = add nsw i32 %2, 1
  store i32 %inc, ptr %i, align 4
  br label %for.cond

for.end:                                          ; preds = %for.cond
  ret void
}

; void assign_pointers_loop() {
;   int x, y, z;
;   int *a1, *a2;
;   while (rand()) {
;     if (rand()) {
;      a2 = &y;
;      a1 = a2;
;      GPtr = a1;
;     } else {
;       a1 = &x;
;       GPtr = a1;
;     }
;
;     if (rand()) {
;       GPtr = &z;
;       return;
;     }
;   }
; }
define dso_local void @assign_pointers_loop() {
; CHECK: Printing analysis 'Escape Analysis' for function 'assign_pointers_loop':
; CHECK-NOT: Escaping objects for BB entry:
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  %z = alloca i32, align 4
  %a1 = alloca ptr, align 8
  %a2 = alloca ptr, align 8
  br label %while.cond

; CHECK: Escaping objects for BB while.cond:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
while.cond:
  %call = call i32 @rand() #2
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %while.body, label %while.end

; CHECK: Escaping objects for BB while.body:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
while.body:
  %call1 = call i32 @rand() #2
  %tobool2 = icmp ne i32 %call1, 0
  br i1 %tobool2, label %if.then, label %if.else

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
if.then:
  store ptr %y, ptr %a2, align 8
  %0 = load ptr, ptr %a2, align 8
  store ptr %0, ptr %a1, align 8
  %1 = load ptr, ptr %a1, align 8
  store ptr %1, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.else:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
if.else:
  store ptr %x, ptr %a1, align 8
  %2 = load ptr, ptr %a1, align 8
  store ptr %2, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
if.end:
  %call3 = call i32 @rand() #2
  %tobool4 = icmp ne i32 %call3, 0
  br i1 %tobool4, label %if.then5, label %if.end6

; CHECK: Escaping objects for BB if.then5:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK-DAG:   %z = alloca i32, align 4
if.then5:
  store ptr %z, ptr @GPtr, align 8
  br label %while.end

; CHECK: Escaping objects for BB if.end6:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
if.end6:
  br label %while.cond

; CHECK: Escaping objects for BB while.end:
; CHECK-DAG:   %y = alloca i32, align 4
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK-DAG:   %z = alloca i32, align 4
while.end:
  ret void
}

declare i32 @rand()