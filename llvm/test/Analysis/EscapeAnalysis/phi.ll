; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8

; Nested PHIs:
;
; void nested_phi() {
; 	int x, y, z;
; 	int *p = malloc(sizeof(int)), *q = &z;
; 	func(&q);
; 	int *pqAlias = NULL;
;
; 	if (rand()) {
; 		if (rand()) {
; 			p = &x;
; 		} else {
; 			p = &y;
; 		}
;
; 		pqAlias = p;
; 	} else {
; 		pqAlias = q;
; 	}
; 	int *pqAliasAlias = pqAlias;
; 	func(&pqAliasAlias);		// to supress pAlias propagation
; }
define dso_local void @nested_phi() #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'nested_phi':
; CHECK: Escaping objects for BB entry:
; CHECK-DAG: %z = alloca i32, align 4
; CHECK-DAG: %q = alloca ptr, align 8
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  %z = alloca i32, align 4
  %q = alloca ptr, align 8
  %pqAliasAlias = alloca ptr, align 8
  %call = call noalias ptr @malloc(i64 noundef 4) #4
  store ptr %z, ptr %q, align 8
  call void @func(ptr noundef %q)
  %call1 = call i32 @rand() #5
  %tobool = icmp ne i32 %call1, 0
  br i1 %tobool, label %if.then, label %if.else5

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG: %z = alloca i32, align 4
; CHECK-DAG: %q = alloca ptr, align 8
if.then:                                          ; preds = %entry
  %call2 = call i32 @rand() #5
  %tobool3 = icmp ne i32 %call2, 0
  br i1 %tobool3, label %if.then4, label %if.else

; CHECK: Escaping objects for BB if.then4:
; CHECK-DAG: %z = alloca i32, align 4
; CHECK-DAG: %q = alloca ptr, align 8
if.then4:                                         ; preds = %if.then
  br label %if.end

; CHECK: Escaping objects for BB if.else:
; CHECK-DAG: %z = alloca i32, align 4
; CHECK-DAG: %q = alloca ptr, align 8
if.else:                                          ; preds = %if.then
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG: %z = alloca i32, align 4
; CHECK-DAG: %q = alloca ptr, align 8
if.end:                                           ; preds = %if.else, %if.then4
  %p.0 = phi ptr [ %x, %if.then4 ], [ %y, %if.else ]
  br label %if.end6

; CHECK: Escaping objects for BB if.else5:
; CHECK-DAG: %z = alloca i32, align 4
; CHECK-DAG: %q = alloca ptr, align 8
if.else5:                                         ; preds = %entry
  %0 = load ptr, ptr %q, align 8
  br label %if.end6

; CHECK: Escaping objects for BB if.end6:
; CHECK-DAG: %pqAliasAlias = alloca ptr, align 8
; CHECK-DAG: %x = alloca i32, align 4
; CHECK-DAG: %y = alloca i32, align 4
; CHECK-DAG: %z = alloca i32, align 4
; CHECK-DAG: %q = alloca ptr, align 8
if.end6:                                          ; preds = %if.else5, %if.end
  %pqAlias.0 = phi ptr [ %p.0, %if.end ], [ %0, %if.else5 ]
  store ptr %pqAlias.0, ptr %pqAliasAlias, align 8
  call void @func(ptr noundef %pqAliasAlias)
  ret void
}

; Simple example with phi node
;
; void escaping_phi() {
; 	int x, y;
; 	int *p;
;
; 	if (GV) {
; 		p = &x;
; 	} else {
; 		p = &y;
; 	}
;
; 	GPtr = p;
; }
define dso_local void @escaping_phi() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escaping_phi':
; CHECK-NOT: Escaping objects for BB entry:
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  %call = call i32 @rand()
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.else

; CHECK-NOT: Escaping objects for BB if.then:
if.then:                                          ; preds = %entry
  br label %if.end

; CHECK-NOT: Escaping objects for BB if.else:
if.else:                                          ; preds = %entry
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:  %x = alloca i32, align 4
; CHECK-DAG:  %y = alloca i32, align 4
if.end:                                           ; preds = %if.else, %if.then
  %p.0 = phi ptr [ %x, %if.then ], [ %y, %if.else ]
  store ptr %p.0, ptr @GPtr, align 8
  ret void
}

declare noalias ptr @malloc(i64 noundef)
declare void @func(ptr noundef)
declare i32 @rand()