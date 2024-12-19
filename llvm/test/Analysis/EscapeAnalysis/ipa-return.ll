; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

; // C code:
; // Here we have functions which return escaped/non-escaped value
; // and other function which uses these functions
;
; int *ret_non_escaping_value() {
;   int *x;
;   return x;
; }
;
; int *ret_escaping_value() {
;   int *y;
;   GPtr = y;
;   return y;
; }
;
; void func() {
;   int *x = ret_non_escaping_value();
;   int *y = ret_escaping_value();
;   int *z = malloc(sizeof(int));
;   int *p = realloc(NULL, sizeof(int));
; }

; CHECK: Printing analysis 'Escape Analysis' for module '<stdin>':

@GPtr = dso_local global ptr null, align 8

define dso_local ptr @ret_non_escaping_value() {
; CHECK: Printing analysis 'Escape Analysis' for function 'ret_non_escaping_value':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %x = alloca ptr, align 8
entry:
  %x = alloca ptr, align 8
  %0 = load ptr, ptr %x, align 8
  ret ptr %0
}

define dso_local ptr @ret_escaping_value() {
; CHECK: Printing analysis 'Escape Analysis' for function 'ret_escaping_value':
; CHECK: Escaping objects for BB entry:
; CHECK-DAG:   %y = alloca ptr, align 8
entry:
  %y = alloca ptr, align 8
  %0 = load ptr, ptr %y, align 8
  store ptr %0, ptr @GPtr, align 8
  %1 = load ptr, ptr %y, align 8
  ret ptr %1
}

define dso_local void @func() {
; CHECK: Printing analysis 'Escape Analysis' for function 'func':
; CHECK: Escaping objects for BB entry:
; CHECK-DAG:  %call1 = call ptr @ret_escaping_value()
; CHECK-DAG:  %z = alloca ptr, align 8
; CHECK-DAG:  %y = alloca ptr, align 8
; CHECK-DAG:  %call2 = call noalias ptr @malloc(i64 noundef 4)
entry:
  %x = alloca ptr, align 8
  %y = alloca ptr, align 8
  %z = alloca ptr, align 8
  %p = alloca ptr, align 8
  %call = call ptr @ret_non_escaping_value()
  store ptr %call, ptr %x, align 8
  %call1 = call ptr @ret_escaping_value()
  store ptr %call1, ptr %y, align 8
  %call2 = call noalias ptr @malloc(i64 noundef 4)
  store ptr %call2, ptr %z, align 8
  %call3 = call ptr @realloc(ptr noundef null, i64 noundef 4)
  store ptr %call3, ptr %p, align 8
  ret void
}

declare noalias ptr @malloc(i64 noundef)
declare ptr @realloc(ptr noundef, i64 noundef)