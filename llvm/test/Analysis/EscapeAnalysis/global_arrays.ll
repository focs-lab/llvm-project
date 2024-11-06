; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@GStr = dso_local global [12 x i8] c"abracadabra\00", align 1
@PtrArr = dso_local global [10 x ptr] zeroinitializer, align 16

; C program:
;
; void escape_through_const_expr_GEP() {
;  	GStr[3] = 'z';		// no escape
;  	int x;
;  	PtrArr[5] = &x;		// escape
; }
define dso_local void @escape_through_const_expr_GEP() {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_through_const_expr_GEP':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:   %x = alloca i32, align 4
entry:
  %x = alloca i32, align 4
  store i8 111, ptr getelementptr inbounds ([12 x i8], ptr @GStr, i64 0, i64 3), align 1
  store ptr %x, ptr getelementptr inbounds ([10 x ptr], ptr @PtrArr, i64 0, i64 5), align 8
  ret void
}