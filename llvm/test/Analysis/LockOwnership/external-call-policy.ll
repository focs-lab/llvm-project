; What a call whose body is not visible may release. An externally visible
; mutex can be unlocked by any function in another translation unit, so an
; opaque call ends its critical section (@ext_data unprotected). A private
; mutex -- local linkage, every use of its address a lock/unlock operand --
; can only be released by code in this module, and an opaque call can reach
; that code only through a callback: nothing here unlocks @priv from a
; function an opaque call could invoke, so the opaque call leaves it held
; (@priv_data protected, also across an indirect call). @cb_priv is private
; too, but its address-taken releaser @cb is reachable from an opaque call,
; so the call may release it (@cb_data unprotected). A library function
; without a callback parameter is transparent (@libc_data protected). An
; indirect call is opaque for an external mutex (@ind_data unprotected). The
; workers themselves are only thread start routines: a new thread cannot
; release the creating thread's mutex, so they are not callback candidates.

; RUN: opt < %s -passes='print<lock-ownership>' -disable-output 2>&1 | FileCheck %s --implicit-check-not='* ext_data' --implicit-check-not='* cb_data' --implicit-check-not='* ind_data'

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@ext = global i64 0, align 8
@priv = internal global i64 0, align 8
@cb_priv = internal global i64 0, align 8
@ext_data = internal global i32 0, align 4
@priv_data = internal global i32 0, align 4
@cb_data = internal global i32 0, align 4
@libc_data = internal global i32 0, align 4
@ind_data = internal global i32 0, align 4
@fp = internal global ptr null, align 8

declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @pthread_create(ptr, ptr, ptr, ptr)
declare void @opaque()
declare i64 @strlen(ptr)

define internal void @cb() {
  %u = call i32 @pthread_mutex_unlock(ptr @cb_priv)
  ret void
}

; --- @ext_data: external mutex across an opaque call ---
define internal ptr @ext_a(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @ext)
  call void @opaque()
  store i32 1, ptr @ext_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @ext)
  ret ptr null
}
define internal ptr @ext_b(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @ext)
  store i32 2, ptr @ext_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @ext)
  ret ptr null
}

; --- @priv_data: private mutex across an opaque call and an indirect call ---
define internal ptr @priv_a(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @priv)
  call void @opaque()
  store i32 1, ptr @priv_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @priv)
  ret ptr null
}
define internal ptr @priv_b(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @priv)
  %f = load ptr, ptr @fp, align 8
  call void %f()
  store i32 2, ptr @priv_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @priv)
  ret ptr null
}

; --- @cb_data: private mutex whose releaser is an address-taken callback ---
define internal ptr @cb_a(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @cb_priv)
  call void @opaque()
  store i32 1, ptr @cb_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @cb_priv)
  ret ptr null
}
define internal ptr @cb_b(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @cb_priv)
  store i32 2, ptr @cb_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @cb_priv)
  ret ptr null
}

; --- @libc_data: external mutex across a transparent library call ---
define internal ptr @libc_a(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @ext)
  %n = call i64 @strlen(ptr %a)
  store i32 1, ptr @libc_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @ext)
  ret ptr null
}
define internal ptr @libc_b(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @ext)
  store i32 2, ptr @libc_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @ext)
  ret ptr null
}

; --- @ind_data: external mutex across an indirect call ---
define internal ptr @ind_a(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @ext)
  %f = load ptr, ptr @fp, align 8
  call void %f()
  store i32 1, ptr @ind_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @ext)
  ret ptr null
}
define internal ptr @ind_b(ptr %a) {
  %l = call i32 @pthread_mutex_lock(ptr @ext)
  store i32 2, ptr @ind_data, align 4
  %u = call i32 @pthread_mutex_unlock(ptr @ext)
  ret ptr null
}

define i32 @main() {
  %t = alloca i64, align 8
  store ptr @cb, ptr @fp, align 8
  %c1 = call i32 @pthread_create(ptr %t, ptr null, ptr @ext_a, ptr null)
  %c2 = call i32 @pthread_create(ptr %t, ptr null, ptr @ext_b, ptr null)
  %c3 = call i32 @pthread_create(ptr %t, ptr null, ptr @priv_a, ptr null)
  %c4 = call i32 @pthread_create(ptr %t, ptr null, ptr @priv_b, ptr null)
  %c5 = call i32 @pthread_create(ptr %t, ptr null, ptr @cb_a, ptr null)
  %c6 = call i32 @pthread_create(ptr %t, ptr null, ptr @cb_b, ptr null)
  %c7 = call i32 @pthread_create(ptr %t, ptr null, ptr @libc_a, ptr null)
  %c8 = call i32 @pthread_create(ptr %t, ptr null, ptr @libc_b, ptr null)
  %c9 = call i32 @pthread_create(ptr %t, ptr null, ptr @ind_a, ptr null)
  %c10 = call i32 @pthread_create(ptr %t, ptr null, ptr @ind_b, ptr null)
  ret i32 0
}

; CHECK: Protected Global Variables
; CHECK-DAG: * priv_data
; CHECK-DAG: * libc_data
