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

; Aliases, and escaping in one of two branches.
;
; Original C code:
; void mutual_aliases_cond() {
; 	int x;
; 	int *a1, *a2;
; 	if (rand()) {
; 		a1 = a2;
; 		GPtr = a2;
; 	} else {
; 		a1 = &x;
; 		x = 999;
; 	}
; }
define dso_local void @mutual_aliases_cond() #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'mutual_aliases_cond':
; CHECK-NOT: Escaping objects for BB entry:
entry:
  %x = alloca i32, align 4
  %a1 = alloca ptr, align 8
  %a2 = alloca ptr, align 8
  %call = call i32 @rand() #2
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.else

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG:  %a1 = alloca ptr, align 8
; CHECK-DAG:  %a2 = alloca ptr, align 8
if.then:                                          ; preds = %entry
  %0 = load ptr, ptr %a2, align 8
  store ptr %0, ptr %a1, align 8
  %1 = load ptr, ptr %a2, align 8
  store ptr %1, ptr @GPtr, align 8
  br label %if.end

; CHECK-NOT: Escaping objects for BB if.else:
if.else:                                          ; preds = %entry
  store ptr %x, ptr %a1, align 8
  store i32 999, ptr %x, align 4
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:  %a1 = alloca ptr, align 8
; CHECK-DAG:  %a2 = alloca ptr, align 8
if.end:                                           ; preds = %if.else, %if.then
  ret void
}

; Here we added the loop, so escape state propagated through other BBs:
;
; void mutual_aliases_loop() {
;  	int x;
;  	int *a1, *a2;
;  	while (rand()) {
;  		if (rand()) {
;  			a1 = a2;
;  			GPtr = a2;
;  		} else {
;  			a1 = &x;
;  			x = 999;
;  		}
;  	}
;  }
define dso_local void @mutual_aliases_loop() {
; CHECK: Printing analysis 'Escape Analysis' for function 'mutual_aliases_loop':
; CHECK-NOT: Escaping objects for BB entry:
entry:
  %x = alloca i32, align 4
  %a1 = alloca ptr, align 8
  %a2 = alloca ptr, align 8
  br label %while.cond

; CHECK: Escaping objects for BB while.cond:
; CHECK-DAG:  %a2 = alloca ptr, align 8
; CHECK-DAG:  %x = alloca i32, align 4
; CHECK-DAG:  %a1 = alloca ptr, align 8
while.cond:                                       ; preds = %if.end, %entry
  %call = call i32 @rand() #2
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %while.body, label %while.end

; CHECK: Escaping objects for BB while.body:
; CHECK-DAG:  %a2 = alloca ptr, align 8
; CHECK-DAG:  %x = alloca i32, align 4
; CHECK-DAG:  %a1 = alloca ptr, align 8
while.body:                                       ; preds = %while.cond
  %call1 = call i32 @rand() #2
  %tobool2 = icmp ne i32 %call1, 0
  br i1 %tobool2, label %if.then, label %if.else

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG:  %a2 = alloca ptr, align 8
; CHECK-DAG:  %x = alloca i32, align 4
; CHECK-DAG:  %a1 = alloca ptr, align 8
if.then:                                          ; preds = %while.body
  %0 = load ptr, ptr %a2, align 8
  store ptr %0, ptr %a1, align 8
  %1 = load ptr, ptr %a2, align 8
  store ptr %1, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.else:
; CHECK-DAG:  %a2 = alloca ptr, align 8
; CHECK-DAG:  %x = alloca i32, align 4
; CHECK-DAG:  %a1 = alloca ptr, align 8
if.else:                                          ; preds = %while.body
  store ptr %x, ptr %a1, align 8
  store i32 999, ptr %x, align 4
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:  %a2 = alloca ptr, align 8
; CHECK-DAG:  %x = alloca i32, align 4
; CHECK-DAG:  %a1 = alloca ptr, align 8
if.end:                                           ; preds = %if.else, %if.then
  br label %while.cond

; CHECK: Escaping objects for BB while.end:
; CHECK-DAG:  %a2 = alloca ptr, align 8
; CHECK-DAG:  %x = alloca i32, align 4
; CHECK-DAG:  %a1 = alloca ptr, align 8
while.end:                                        ; preds = %while.cond
  ret void
}

declare i32 @rand()