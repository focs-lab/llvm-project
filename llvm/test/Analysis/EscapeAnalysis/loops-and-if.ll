; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8

define dso_local void @escape_in_conditional() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_in_conditional':
; CHECK-NEXT: Escaping variables:
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
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %x = alloca i32, align 4
; CHECK-DAG:   %i = alloca i32, align 4
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