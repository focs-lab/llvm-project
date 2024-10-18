; RUN: opt < %s -passes='print<escape-analysis>' -disable-output 2>&1 | FileCheck %s

%struct.__va_list_tag = type { i32, i32, ptr, ptr }

@VoidGPtr = dso_local global ptr null, align 8
@GPtr = dso_local global ptr null, align 8
@GV = dso_local global i32 0, align 4
@Ptr = dso_local global ptr null, align 8

; Function Attrs: noinline nounwind optnone uwtable
define dso_local void @phi_aliasing(ptr noundef %str, i32 noundef %n, ...) #0 {
; CHECK: Printing analysis 'Escape Analysis' for function 'phi_aliasing':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %p = alloca ptr, align 8
entry:
  %str.addr = alloca ptr, align 8
  %n.addr = alloca i32, align 4
  %ap = alloca [1 x %struct.__va_list_tag], align 16
  %p = alloca ptr, align 8
  store ptr %str, ptr %str.addr, align 8
  store i32 %n, ptr %n.addr, align 4
  %arraydecay = getelementptr inbounds [1 x %struct.__va_list_tag], ptr %ap, i64 0, i64 0
  %gp_offset_p = getelementptr inbounds %struct.__va_list_tag, ptr %arraydecay, i32 0, i32 0
  %gp_offset = load i32, ptr %gp_offset_p, align 16
  %fits_in_gp = icmp ule i32 %gp_offset, 40
  br i1 %fits_in_gp, label %vaarg.in_reg, label %vaarg.in_mem

vaarg.in_reg:                                     ; preds = %entry
  %0 = getelementptr inbounds %struct.__va_list_tag, ptr %arraydecay, i32 0, i32 3
  %reg_save_area = load ptr, ptr %0, align 16
  %1 = getelementptr i8, ptr %reg_save_area, i32 %gp_offset
  %2 = add i32 %gp_offset, 8
  store i32 %2, ptr %gp_offset_p, align 16
  br label %vaarg.end

vaarg.in_mem:                                     ; preds = %entry
  %overflow_arg_area_p = getelementptr inbounds %struct.__va_list_tag, ptr %arraydecay, i32 0, i32 2
  %overflow_arg_area = load ptr, ptr %overflow_arg_area_p, align 8
  %overflow_arg_area.next = getelementptr i8, ptr %overflow_arg_area, i32 8
  store ptr %overflow_arg_area.next, ptr %overflow_arg_area_p, align 8
  br label %vaarg.end

vaarg.end:                                        ; preds = %vaarg.in_mem, %vaarg.in_reg
  %vaarg.addr = phi ptr [ %1, %vaarg.in_reg ], [ %overflow_arg_area, %vaarg.in_mem ]
  %3 = load ptr, ptr %vaarg.addr, align 8
  store ptr %3, ptr %p, align 8
  %4 = load ptr, ptr %p, align 8
  store ptr %4, ptr @VoidGPtr, align 8
  ret void
}

define dso_local void @phi_aliasing_with_loop(ptr noundef %str, i32 noundef %n, ...) {
; CHECK: Printing analysis 'Escape Analysis' for function 'phi_aliasing_with_loop':
; CHECK-NEXT: Escaping variables:
; CHECK-DAG:   %p = alloca ptr, align 8
entry:
  %str.addr = alloca ptr, align 8
  %n.addr = alloca i32, align 4
  %ap = alloca [1 x %struct.__va_list_tag], align 16
  %p = alloca ptr, align 8
  store ptr %str, ptr %str.addr, align 8
  store i32 %n, ptr %n.addr, align 4
  br label %while.cond

while.cond:                                       ; preds = %vaarg.end, %entry
  %0 = load i32, ptr %n.addr, align 4
  %dec = add nsw i32 %0, -1
  store i32 %dec, ptr %n.addr, align 4
  %tobool = icmp ne i32 %0, 0
  br i1 %tobool, label %while.body, label %while.end

while.body:                                       ; preds = %while.cond
  %arraydecay = getelementptr inbounds [1 x %struct.__va_list_tag], ptr %ap, i64 0, i64 0
  %gp_offset_p = getelementptr inbounds %struct.__va_list_tag, ptr %arraydecay, i32 0, i32 0
  %gp_offset = load i32, ptr %gp_offset_p, align 16
  %fits_in_gp = icmp ule i32 %gp_offset, 40
  br i1 %fits_in_gp, label %vaarg.in_reg, label %vaarg.in_mem

vaarg.in_reg:                                     ; preds = %while.body
  %1 = getelementptr inbounds %struct.__va_list_tag, ptr %arraydecay, i32 0, i32 3
  %reg_save_area = load ptr, ptr %1, align 16
  %2 = getelementptr i8, ptr %reg_save_area, i32 %gp_offset
  %3 = add i32 %gp_offset, 8
  store i32 %3, ptr %gp_offset_p, align 16
  br label %vaarg.end

vaarg.in_mem:                                     ; preds = %while.body
  %overflow_arg_area_p = getelementptr inbounds %struct.__va_list_tag, ptr %arraydecay, i32 0, i32 2
  %overflow_arg_area = load ptr, ptr %overflow_arg_area_p, align 8
  %overflow_arg_area.next = getelementptr i8, ptr %overflow_arg_area, i32 8
  store ptr %overflow_arg_area.next, ptr %overflow_arg_area_p, align 8
  br label %vaarg.end

vaarg.end:                                        ; preds = %vaarg.in_mem, %vaarg.in_reg
  %vaarg.addr = phi ptr [ %2, %vaarg.in_reg ], [ %overflow_arg_area, %vaarg.in_mem ]
  %4 = load ptr, ptr %vaarg.addr, align 8
  store ptr %4, ptr %p, align 8
  br label %while.cond

while.end:                                        ; preds = %while.cond
  %5 = load ptr, ptr %p, align 8
  store ptr %5, ptr @VoidGPtr, align 8
  ret void
}