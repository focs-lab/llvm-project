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

@GV = dso_local global i32 0, align 4

define dso_local void @func(i32 noundef %v) {
  ret void
}

define dso_local i32 @passing_value_is_not_escape() #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'passing_value_is_not_escape':
; CHECK-NOT: Escaping variables:
entry:
  %x = alloca i32, align 4
  %0 = load i32, ptr %x, align 4
  store i32 %0, ptr @GV, align 4
  %1 = load i32, ptr %x, align 4
  call void @func(i32 noundef %1)
  %2 = load i32, ptr %x, align 4
  ret i32 %2
}