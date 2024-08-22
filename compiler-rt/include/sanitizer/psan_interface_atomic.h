//===-- psan_interface_atomic.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (PSan), a race detector.
//
// Public interface header for PSan atomics.
//===----------------------------------------------------------------------===//
#ifndef PSAN_INTERFACE_ATOMIC_H
#define PSAN_INTERFACE_ATOMIC_H

#include <sanitizer/common_interface_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef char __psan_atomic8;
typedef short __psan_atomic16;
typedef int __psan_atomic32;
typedef long __psan_atomic64;
#if defined(__SIZEOF_INT128__) ||                                              \
    (__clang_major__ * 100 + __clang_minor__ >= 302)
__extension__ typedef __int128 __psan_atomic128;
#define __PSAN_HAS_INT128 1
#else
#define __PSAN_HAS_INT128 0
#endif

// Part of ABI, do not change.
// https://github.com/llvm/llvm-project/blob/main/libcxx/include/atomic
typedef enum {
  __psan_memory_order_relaxed,
  __psan_memory_order_consume,
  __psan_memory_order_acquire,
  __psan_memory_order_release,
  __psan_memory_order_acq_rel,
  __psan_memory_order_seq_cst
} __psan_memory_order;

__psan_atomic8 SANITIZER_CDECL
__psan_atomic8_load(const volatile __psan_atomic8 *a, __psan_memory_order mo);
__psan_atomic16 SANITIZER_CDECL
__psan_atomic16_load(const volatile __psan_atomic16 *a, __psan_memory_order mo);
__psan_atomic32 SANITIZER_CDECL
__psan_atomic32_load(const volatile __psan_atomic32 *a, __psan_memory_order mo);
__psan_atomic64 SANITIZER_CDECL
__psan_atomic64_load(const volatile __psan_atomic64 *a, __psan_memory_order mo);
#if __PSAN_HAS_INT128
__psan_atomic128 SANITIZER_CDECL __psan_atomic128_load(
    const volatile __psan_atomic128 *a, __psan_memory_order mo);
#endif

void SANITIZER_CDECL __psan_atomic8_store(volatile __psan_atomic8 *a,
                                          __psan_atomic8 v,
                                          __psan_memory_order mo);
void SANITIZER_CDECL __psan_atomic16_store(volatile __psan_atomic16 *a,
                                           __psan_atomic16 v,
                                           __psan_memory_order mo);
void SANITIZER_CDECL __psan_atomic32_store(volatile __psan_atomic32 *a,
                                           __psan_atomic32 v,
                                           __psan_memory_order mo);
void SANITIZER_CDECL __psan_atomic64_store(volatile __psan_atomic64 *a,
                                           __psan_atomic64 v,
                                           __psan_memory_order mo);
#if __PSAN_HAS_INT128
void SANITIZER_CDECL __psan_atomic128_store(volatile __psan_atomic128 *a,
                                            __psan_atomic128 v,
                                            __psan_memory_order mo);
#endif

__psan_atomic8 SANITIZER_CDECL __psan_atomic8_exchange(
    volatile __psan_atomic8 *a, __psan_atomic8 v, __psan_memory_order mo);
__psan_atomic16 SANITIZER_CDECL __psan_atomic16_exchange(
    volatile __psan_atomic16 *a, __psan_atomic16 v, __psan_memory_order mo);
__psan_atomic32 SANITIZER_CDECL __psan_atomic32_exchange(
    volatile __psan_atomic32 *a, __psan_atomic32 v, __psan_memory_order mo);
__psan_atomic64 SANITIZER_CDECL __psan_atomic64_exchange(
    volatile __psan_atomic64 *a, __psan_atomic64 v, __psan_memory_order mo);
#if __PSAN_HAS_INT128
__psan_atomic128 SANITIZER_CDECL __psan_atomic128_exchange(
    volatile __psan_atomic128 *a, __psan_atomic128 v, __psan_memory_order mo);
#endif

__psan_atomic8 SANITIZER_CDECL __psan_atomic8_fetch_add(
    volatile __psan_atomic8 *a, __psan_atomic8 v, __psan_memory_order mo);
__psan_atomic16 SANITIZER_CDECL __psan_atomic16_fetch_add(
    volatile __psan_atomic16 *a, __psan_atomic16 v, __psan_memory_order mo);
__psan_atomic32 SANITIZER_CDECL __psan_atomic32_fetch_add(
    volatile __psan_atomic32 *a, __psan_atomic32 v, __psan_memory_order mo);
__psan_atomic64 SANITIZER_CDECL __psan_atomic64_fetch_add(
    volatile __psan_atomic64 *a, __psan_atomic64 v, __psan_memory_order mo);
#if __PSAN_HAS_INT128
__psan_atomic128 SANITIZER_CDECL __psan_atomic128_fetch_add(
    volatile __psan_atomic128 *a, __psan_atomic128 v, __psan_memory_order mo);
#endif

__psan_atomic8 SANITIZER_CDECL __psan_atomic8_fetch_sub(
    volatile __psan_atomic8 *a, __psan_atomic8 v, __psan_memory_order mo);
__psan_atomic16 SANITIZER_CDECL __psan_atomic16_fetch_sub(
    volatile __psan_atomic16 *a, __psan_atomic16 v, __psan_memory_order mo);
__psan_atomic32 SANITIZER_CDECL __psan_atomic32_fetch_sub(
    volatile __psan_atomic32 *a, __psan_atomic32 v, __psan_memory_order mo);
__psan_atomic64 SANITIZER_CDECL __psan_atomic64_fetch_sub(
    volatile __psan_atomic64 *a, __psan_atomic64 v, __psan_memory_order mo);
#if __PSAN_HAS_INT128
__psan_atomic128 SANITIZER_CDECL __psan_atomic128_fetch_sub(
    volatile __psan_atomic128 *a, __psan_atomic128 v, __psan_memory_order mo);
#endif

__psan_atomic8 SANITIZER_CDECL __psan_atomic8_fetch_and(
    volatile __psan_atomic8 *a, __psan_atomic8 v, __psan_memory_order mo);
__psan_atomic16 SANITIZER_CDECL __psan_atomic16_fetch_and(
    volatile __psan_atomic16 *a, __psan_atomic16 v, __psan_memory_order mo);
__psan_atomic32 SANITIZER_CDECL __psan_atomic32_fetch_and(
    volatile __psan_atomic32 *a, __psan_atomic32 v, __psan_memory_order mo);
__psan_atomic64 SANITIZER_CDECL __psan_atomic64_fetch_and(
    volatile __psan_atomic64 *a, __psan_atomic64 v, __psan_memory_order mo);
#if __PSAN_HAS_INT128
__psan_atomic128 SANITIZER_CDECL __psan_atomic128_fetch_and(
    volatile __psan_atomic128 *a, __psan_atomic128 v, __psan_memory_order mo);
#endif

__psan_atomic8 SANITIZER_CDECL __psan_atomic8_fetch_or(
    volatile __psan_atomic8 *a, __psan_atomic8 v, __psan_memory_order mo);
__psan_atomic16 SANITIZER_CDECL __psan_atomic16_fetch_or(
    volatile __psan_atomic16 *a, __psan_atomic16 v, __psan_memory_order mo);
__psan_atomic32 SANITIZER_CDECL __psan_atomic32_fetch_or(
    volatile __psan_atomic32 *a, __psan_atomic32 v, __psan_memory_order mo);
__psan_atomic64 SANITIZER_CDECL __psan_atomic64_fetch_or(
    volatile __psan_atomic64 *a, __psan_atomic64 v, __psan_memory_order mo);
#if __PSAN_HAS_INT128
__psan_atomic128 SANITIZER_CDECL __psan_atomic128_fetch_or(
    volatile __psan_atomic128 *a, __psan_atomic128 v, __psan_memory_order mo);
#endif

__psan_atomic8 SANITIZER_CDECL __psan_atomic8_fetch_xor(
    volatile __psan_atomic8 *a, __psan_atomic8 v, __psan_memory_order mo);
__psan_atomic16 SANITIZER_CDECL __psan_atomic16_fetch_xor(
    volatile __psan_atomic16 *a, __psan_atomic16 v, __psan_memory_order mo);
__psan_atomic32 SANITIZER_CDECL __psan_atomic32_fetch_xor(
    volatile __psan_atomic32 *a, __psan_atomic32 v, __psan_memory_order mo);
__psan_atomic64 SANITIZER_CDECL __psan_atomic64_fetch_xor(
    volatile __psan_atomic64 *a, __psan_atomic64 v, __psan_memory_order mo);
#if __PSAN_HAS_INT128
__psan_atomic128 SANITIZER_CDECL __psan_atomic128_fetch_xor(
    volatile __psan_atomic128 *a, __psan_atomic128 v, __psan_memory_order mo);
#endif

__psan_atomic8 SANITIZER_CDECL __psan_atomic8_fetch_nand(
    volatile __psan_atomic8 *a, __psan_atomic8 v, __psan_memory_order mo);
__psan_atomic16 SANITIZER_CDECL __psan_atomic16_fetch_nand(
    volatile __psan_atomic16 *a, __psan_atomic16 v, __psan_memory_order mo);
__psan_atomic32 SANITIZER_CDECL __psan_atomic32_fetch_nand(
    volatile __psan_atomic32 *a, __psan_atomic32 v, __psan_memory_order mo);
__psan_atomic64 SANITIZER_CDECL __psan_atomic64_fetch_nand(
    volatile __psan_atomic64 *a, __psan_atomic64 v, __psan_memory_order mo);
#if __PSAN_HAS_INT128
__psan_atomic128 SANITIZER_CDECL __psan_atomic128_fetch_nand(
    volatile __psan_atomic128 *a, __psan_atomic128 v, __psan_memory_order mo);
#endif

int SANITIZER_CDECL __psan_atomic8_compare_exchange_weak(
    volatile __psan_atomic8 *a, __psan_atomic8 *c, __psan_atomic8 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
int SANITIZER_CDECL __psan_atomic16_compare_exchange_weak(
    volatile __psan_atomic16 *a, __psan_atomic16 *c, __psan_atomic16 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
int SANITIZER_CDECL __psan_atomic32_compare_exchange_weak(
    volatile __psan_atomic32 *a, __psan_atomic32 *c, __psan_atomic32 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
int SANITIZER_CDECL __psan_atomic64_compare_exchange_weak(
    volatile __psan_atomic64 *a, __psan_atomic64 *c, __psan_atomic64 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
#if __PSAN_HAS_INT128
int SANITIZER_CDECL __psan_atomic128_compare_exchange_weak(
    volatile __psan_atomic128 *a, __psan_atomic128 *c, __psan_atomic128 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
#endif

int SANITIZER_CDECL __psan_atomic8_compare_exchange_strong(
    volatile __psan_atomic8 *a, __psan_atomic8 *c, __psan_atomic8 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
int SANITIZER_CDECL __psan_atomic16_compare_exchange_strong(
    volatile __psan_atomic16 *a, __psan_atomic16 *c, __psan_atomic16 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
int SANITIZER_CDECL __psan_atomic32_compare_exchange_strong(
    volatile __psan_atomic32 *a, __psan_atomic32 *c, __psan_atomic32 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
int SANITIZER_CDECL __psan_atomic64_compare_exchange_strong(
    volatile __psan_atomic64 *a, __psan_atomic64 *c, __psan_atomic64 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
#if __PSAN_HAS_INT128
int SANITIZER_CDECL __psan_atomic128_compare_exchange_strong(
    volatile __psan_atomic128 *a, __psan_atomic128 *c, __psan_atomic128 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
#endif

__psan_atomic8 SANITIZER_CDECL __psan_atomic8_compare_exchange_val(
    volatile __psan_atomic8 *a, __psan_atomic8 c, __psan_atomic8 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
__psan_atomic16 SANITIZER_CDECL __psan_atomic16_compare_exchange_val(
    volatile __psan_atomic16 *a, __psan_atomic16 c, __psan_atomic16 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
__psan_atomic32 SANITIZER_CDECL __psan_atomic32_compare_exchange_val(
    volatile __psan_atomic32 *a, __psan_atomic32 c, __psan_atomic32 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
__psan_atomic64 SANITIZER_CDECL __psan_atomic64_compare_exchange_val(
    volatile __psan_atomic64 *a, __psan_atomic64 c, __psan_atomic64 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
#if __PSAN_HAS_INT128
__psan_atomic128 SANITIZER_CDECL __psan_atomic128_compare_exchange_val(
    volatile __psan_atomic128 *a, __psan_atomic128 c, __psan_atomic128 v,
    __psan_memory_order mo, __psan_memory_order fail_mo);
#endif

void SANITIZER_CDECL __psan_atomic_thread_fence(__psan_memory_order mo);
void SANITIZER_CDECL __psan_atomic_signal_fence(__psan_memory_order mo);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // PSAN_INTERFACE_ATOMIC_H
