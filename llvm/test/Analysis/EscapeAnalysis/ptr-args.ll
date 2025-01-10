; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

%struct.StructTy = type { ptr }

@GPtr = dso_local global ptr null, align 8

; void escape_through_ptr_argument(int **k) {
;   int x;
;   *k = &x;
; }
;
define dso_local void @escape_through_ptr_argument(ptr noundef %k) {
; CHECK: Printing analysis 'Escape Analysis' for function 'escape_through_ptr_argument':
; CHECK-NEXT: Escaping objects for BB entry:
; CHECK-DAG:  %x = alloca i32, align 4
entry:
  %k.addr = alloca ptr, align 8
  %x = alloca i32, align 4
  store ptr %k, ptr %k.addr, align 8
  %0 = load ptr, ptr %k.addr, align 8
  store ptr %x, ptr %0, align 8
  ret void
}

;void no_escape_through_ptr_argument(int *k) {
;  int x;
;  k = &x;
;}
;
define dso_local void @no_escape_through_ptr_argument(ptr noundef %k) {
; CHECK: Printing analysis 'Escape Analysis' for function 'no_escape_through_ptr_argument':
; CHECK-NOT: Escaping objects for BB entry:
entry:
  %k.addr = alloca ptr, align 8
  %x = alloca i32, align 4
  store ptr %k, ptr %k.addr, align 8
  store ptr %x, ptr %k.addr, align 8
  ret void
}

; void no_escape_through_gptr() {
;   int *GPtrAlias = GPtr;
; }
;
define void @no_escape_through_gptr() {
; CHECK: Printing analysis 'Escape Analysis' for function 'no_escape_through_gptr':
; CHECK-NOT: Escaping objects for BB entry:
entry:
  %GPtrAlias = alloca ptr, align 8
  %0 = load ptr, ptr @GPtr, align 8
  store ptr %0, ptr %GPtrAlias, align 8
  ret void
}