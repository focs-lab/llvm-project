; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

@global_ptr = dso_local global ptr null, align 8

define dso_local void @no_escape_local() #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'no_escape_local':
; CHECK-NOT: Escaping variables:
entry:
  %x = alloca i32, align 4
  store i32 40, ptr %x, align 4
  ret void
}

define dso_local void @no_escape_arg(i32 noundef %x) #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'no_escape_arg':
; CHECK-NOT: Escaping variables:
entry:
  %x.addr = alloca i32, align 4
  store i32 %x, ptr %x.addr, align 4
  ret void
}