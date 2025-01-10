; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8
@GPtrPtr = dso_local global ptr null, align 8

; CHECK: Printing analysis 'Escape Analysis' for module '<stdin>':

; static void rec_func(int **x, int **y, int **z) {
;   int xLocal;
;   *x = &xLocal;
;
;   if (rand()) {
;     rec_func(x, y, z);
;   }
;   GPtrPtr = x;
; }
;
; void caller() {
;   int *x, *y, *z;
;   rec_func(&x, &y, &z);
; }

define dso_local void @caller() {
; CHECK: Printing analysis 'Escape Analysis' for function 'caller':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %x = alloca ptr, align 8
entry:
  %x = alloca ptr, align 8
  %y = alloca ptr, align 8
  %z = alloca ptr, align 8
  call void @rec_func(ptr noundef %x, ptr noundef %y, ptr noundef %z)
  ret void
}

; Function Attrs: noinline nounwind uwtable
define internal void @rec_func(ptr noundef %x, ptr noundef %y, ptr noundef %z) #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'rec_func':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %xLocal = alloca i32, align 4
; CHECK-DAG: ptr %x
entry:
  %x.addr = alloca ptr, align 8
  %y.addr = alloca ptr, align 8
  %z.addr = alloca ptr, align 8
  %xLocal = alloca i32, align 4
  store ptr %x, ptr %x.addr, align 8
  store ptr %y, ptr %y.addr, align 8
  store ptr %z, ptr %z.addr, align 8
  %0 = load ptr, ptr %x.addr, align 8
  store ptr %xLocal, ptr %0, align 8
  %call = call i32 @rand() #2
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.end

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG:   %xLocal = alloca i32, align 4
; CHECK-DAG: ptr %x
if.then:                                          ; preds = %entry
  %1 = load ptr, ptr %x.addr, align 8
  %2 = load ptr, ptr %y.addr, align 8
  %3 = load ptr, ptr %z.addr, align 8
  call void @rec_func(ptr noundef %1, ptr noundef %2, ptr noundef %3)
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG:   %xLocal = alloca i32, align 4
; CHECK-DAG: ptr %x
if.end:                                           ; preds = %if.then, %entry
  %4 = load ptr, ptr %x.addr, align 8
  store ptr %4, ptr @GPtrPtr, align 8
  ret void
}

; SCC of 3 mutually recursive functions:
;
; __attribute_noinline__ void SCC_foo(int *x, int *y, int *z, int *p, int *NoEsc) {
; 	GPtr = x;
; 	SCC_bar(x, y, z, p, NoEsc);
; }
;
; __attribute_noinline__ void SCC_bar(int *x, int *y, int *z, int *p, int *NoEsc) {
; 	GPtr = y;
; 	int *xAlias = x;
; 	int *yAlias = y;
; 	int *zAlias = z;
; 	if (rand())
; 		GPtr = p;
; 	SCC_buz(x, y, z, p, NoEsc);
; }
;
; __attribute_noinline__ void SCC_buz(int *x, int *y, int *z, int *p, int *NoEsc) {
; 	GPtr = z;
; 	SCC_foo(x, y, z, p, NoEsc);
; }

define dso_local void @SCC_foo(ptr noundef %x, ptr noundef %y, ptr noundef %z, ptr noundef %p, ptr noundef %NoEsc) {
; CHECK: Printing analysis 'Escape Analysis' for function 'SCC_foo':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %z
; CHECK-DAG: ptr %p
; CHECK-DAG: ptr %x
; CHECK-DAG: ptr %y
entry:
  store ptr %x, ptr @GPtr, align 8
  call void @SCC_bar(ptr noundef %x, ptr noundef %y, ptr noundef %z, ptr noundef %p, ptr noundef %NoEsc)
  ret void
}

define dso_local void @SCC_bar(ptr noundef %x, ptr noundef %y, ptr noundef %z, ptr noundef %p, ptr noundef %NoEsc) {
; CHECK: Printing analysis 'Escape Analysis' for function 'SCC_bar':
; CHECK: Escaping objects for BB entry:
; CHECK-DAG: ptr %y
entry:
  store ptr %y, ptr @GPtr, align 8
  %call = call i32 @rand() #2
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.end

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG: ptr %p
; CHECK-DAG: ptr %y
if.then:                                          ; preds = %entry
  store ptr %p, ptr @GPtr, align 8
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG: ptr %p
; CHECK-DAG: ptr %z
; CHECK-DAG: ptr %y
; CHECK-DAG: ptr %x
if.end:                                           ; preds = %if.then, %entry
  call void @SCC_buz(ptr noundef %x, ptr noundef %y, ptr noundef %z, ptr noundef %p, ptr noundef %NoEsc)
  ret void
}

declare i32 @rand() #1

define dso_local void @SCC_buz(ptr noundef %x, ptr noundef %y, ptr noundef %z, ptr noundef %p, ptr noundef %NoEsc) {
; CHECK: Printing analysis 'Escape Analysis' for function 'SCC_buz':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG: ptr %z
; CHECK-DAG: ptr %p
; CHECK-DAG: ptr %x
; CHECK-DAG: ptr %y
entry:
  store ptr %z, ptr @GPtr, align 8
  call void @SCC_foo(ptr noundef %x, ptr noundef %y, ptr noundef %z, ptr noundef %p, ptr noundef %NoEsc)
  ret void
}