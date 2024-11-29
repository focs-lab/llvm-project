; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

; C code:
;
; // Simple recursive function
; void rec_func(int *x) {
; 	if (*x) {
; 		(*x)--;
;		rec_func(x);
; 	}
; }
;
; // SCC of 3 mutually recursive functions
; void SCC_foo(int *x);
; void SCC_bar(int *x);
; void SCC_buz(int *x);
;
; void SCC_foo(int *x) {
; 	SCC_bar(x);
; }
;
; void SCC_bar(int *x) {
; 	SCC_buz(x);
; }
;
; void SCC_buz(int *x) {
; 	SCC_foo(x);
; }

; CHECK: Printing analysis 'Escape Analysis' for module '<stdin>':

@GPtr = dso_local global ptr null, align 8

define dso_local void @rec_func(ptr noundef %x) {
; CHECK: Printing analysis 'Escape Analysis' for function 'rec_func':
; CHECK-NEXT: Escaping objects for BB if.then:
; CHECK-DAG: ptr %x
; CHECK: Escaping objects for BB if.end:
; CHECK-DAG: ptr %x
entry:
  %0 = load i32, ptr %x, align 4
  %tobool = icmp ne i32 %0, 0
  br i1 %tobool, label %if.then, label %if.end

if.then:                                          ; preds = %entry
  %1 = load i32, ptr %x, align 4
  %dec = add nsw i32 %1, -1
  store i32 %dec, ptr %x, align 4
  call void @rec_func(ptr noundef %x)
  br label %if.end

if.end:                                           ; preds = %if.then, %entry
  ret void
}

define dso_local void @SCC_foo(ptr noundef %x) {
; CHECK: Printing analysis 'Escape Analysis' for function 'SCC_foo':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %x
entry:
  call void @SCC_bar(ptr noundef %x)
  ret void
}

define dso_local void @SCC_bar(ptr noundef %x) {
; CHECK: Printing analysis 'Escape Analysis' for function 'SCC_bar':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %x
entry:
  call void @SCC_buz(ptr noundef %x)
  ret void
}

define dso_local void @SCC_buz(ptr noundef %x) {
; CHECK: Printing analysis 'Escape Analysis' for function 'SCC_buz':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %x
entry:
  call void @SCC_foo(ptr noundef %x)
  ret void
}