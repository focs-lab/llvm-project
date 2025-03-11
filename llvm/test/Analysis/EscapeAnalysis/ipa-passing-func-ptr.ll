; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

; int main() {
;   int x;
;   pthread_t thr;
;   pthread_create(&thr, 0, thread, &x);
;   pthread_join(thr, 0);
;   return 0;
; }
define dso_local i32 @main() {
; CHECK: Printing analysis 'Escape Analysis' for function 'main':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %thr = alloca i64, align 8
; CHECK-DAG:   %x = alloca i32, align 4
entry:
  %retval = alloca i32, align 4
  %x = alloca i32, align 4
  %thr = alloca i64, align 8
  store i32 0, ptr %retval, align 4
  %call = call i32 @pthread_create(ptr noundef %thr, ptr noundef null, ptr noundef @thread, ptr noundef %x)
  %0 = load i64, ptr %thr, align 8
  %call1 = call i32 @pthread_join(i64 noundef %0, ptr noundef null)
  ret i32 0
}

; Passing function pointer to other function
; static void *thread(void *p) {
;   *(int*)p = 42;
;   return 0;
; }
define internal ptr @thread(ptr noundef %p) {
; CHECK: Printing analysis 'Escape Analysis' for function 'thread':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-NEXT: ptr %p
entry:
  %p.addr = alloca ptr, align 8
  store ptr %p, ptr %p.addr, align 8
  %0 = load ptr, ptr %p.addr, align 8
  store i32 42, ptr %0, align 4
  ret ptr null
}

declare i32 @pthread_create(ptr noundef, ptr noundef, ptr noundef, ptr noundef)
declare i32 @pthread_join(i64 noundef, ptr noundef) #2
