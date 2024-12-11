; RUN: opt < %s -passes='print<escape-analysis-global>' -disable-output 2>&1 | FileCheck %s

@GPtr = dso_local global ptr null, align 8

; CHECK: Printing analysis 'Escape Analysis' for module '<stdin>':

; // Simple recursive function
; void rec_func(int *x, int *y, int *z) {
; 	if (rand()) {
; 		int *Alias = y;
; 		*Alias = 333;
; 	} else {
; 		rec_func(x, y, z);
; 	}
; 	int *Alias2 = x;
; 	GPtr = Alias2;
; 	GPtr = y;
; }

define dso_local void @rec_func(ptr noundef %x, ptr noundef %y, ptr noundef %z) {
; CHECK: Printing analysis 'Escape Analysis' for function 'rec_func':
; CHECK: Escaping objects for BB entry:
; CHECK-DAG: ptr %y
; CHECK-DAG: ptr %x
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %x.addr = alloca ptr, align 8
entry:
  %x.addr = alloca ptr, align 8
  %y.addr = alloca ptr, align 8
  %z.addr = alloca ptr, align 8
  %Alias = alloca ptr, align 8
  %Alias2 = alloca ptr, align 8
  store ptr %x, ptr %x.addr, align 8
  store ptr %y, ptr %y.addr, align 8
  store ptr %z, ptr %z.addr, align 8
  %call = call i32 @rand() #2
  %tobool = icmp ne i32 %call, 0
  br i1 %tobool, label %if.then, label %if.else

; CHECK: Escaping objects for BB if.then:
; CHECK-DAG: ptr %y
; CHECK-DAG:   %Alias = alloca ptr, align 8
; CHECK-DAG: ptr %x
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %x.addr = alloca ptr, align 8
if.then:                                          ; preds = %entry
  %0 = load ptr, ptr %y.addr, align 8
  store ptr %0, ptr %Alias, align 8
  %1 = load ptr, ptr %Alias, align 8
  store i32 333, ptr %1, align 4
  br label %if.end

; CHECK: Escaping objects for BB if.else:
; CHECK-DAG: ptr %y
; CHECK-DAG: ptr %x
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %x.addr = alloca ptr, align 8
if.else:                                          ; preds = %entry
  %2 = load ptr, ptr %x.addr, align 8
  %3 = load ptr, ptr %y.addr, align 8
  %4 = load ptr, ptr %z.addr, align 8
  call void @rec_func(ptr noundef %2, ptr noundef %3, ptr noundef %4)
  br label %if.end

; CHECK: Escaping objects for BB if.end:
; CHECK-DAG: ptr %y
; CHECK-DAG:   %Alias2 = alloca ptr, align 8
; CHECK-DAG:   %Alias = alloca ptr, align 8
; CHECK-DAG: ptr %x
; CHECK-DAG:   %y.addr = alloca ptr, align 8
; CHECK-DAG:   %x.addr = alloca ptr, align 8
if.end:                                           ; preds = %if.else, %if.then
  %5 = load ptr, ptr %x.addr, align 8
  store ptr %5, ptr %Alias2, align 8
  %6 = load ptr, ptr %Alias2, align 8
  store ptr %6, ptr @GPtr, align 8
  %7 = load ptr, ptr %y.addr, align 8
  store ptr %7, ptr @GPtr, align 8
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