//===-- psan_dense_alloc.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PredictiveSanitizer (PSan), a predictive race
// detector. This is a fork of ThreadSanitizer (TSan) at LLVM commit
// c609043dd00955bf177ff57b0bad2a87c1e61a36.
//
//===----------------------------------------------------------------------===//
#ifndef PSAN_SHADOW_ALLOC_H
#define PSAN_SHADOW_ALLOC_H

#include "psan_defs.h"
#include "psan_ilist.h"
#include "sanitizer_common/sanitizer_common.h"

namespace __psan {

static int num_shadow_allocs = 0;
static int num_shadow_alloc_refills = 0;
static int num_shadow_alloc_recycles = 0;

template <typename ShadowClass, uptr size1, uptr size2>
class ShadowAlloc {
 public:
  ShadowAlloc() {
    // num_shadow_allocs++;
    // Printf("ShadowAlloc ctor %u\n", num_shadow_allocs);
    Refill<size1>();
  }

  ~ShadowAlloc() {
    // Printf("ShadowAlloc dtor\n");
  }

  ShadowClass* next() {
    // Printf("this: %p\n", this);
    if (UNLIKELY(shadow_pool_cur == shadow_pool_end))
      Refill<size2>();
    return shadow_pool_cur++;
  }

  INode node;

 private:
  template <uptr size>
  void Refill() {
    // refills++;
    // Printf("ShadowAlloc Refill %u\n", refills);
    shadow_pool_cur = (ShadowClass*) MmapOrDie(size * sizeof(ShadowClass), "ShadowAlloc");
    shadow_pool_end = shadow_pool_cur + size;
  }

  ShadowClass* shadow_pool_cur;
  ShadowClass* shadow_pool_end;
  int refills = 0;
};

}  // namespace __psan

#endif  // PSAN_SHADOW_ALLOC_H
