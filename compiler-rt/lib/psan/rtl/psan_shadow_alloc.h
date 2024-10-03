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

template <typename ShadowCell, uptr size1, uptr size2>
class ShadowAlloc {
 public:
  ShadowAlloc() {
    // num_shadow_allocs++;
    // Printf("ShadowAlloc ctor %u\n", num_shadow_allocs);
    Refill<size1>();
    free_list_ = nullptr;
  }

  ~ShadowAlloc() {
    // Printf("ShadowAlloc dtor\n");
  }

  ShadowCell* next() {
    // Printf("this: %p\n", this);
    if (UNLIKELY(shadow_pool_cur_ == shadow_pool_end_)) {
      if (free_list_ != nullptr) {
        ShadowCell* cur = free_list_;
        // TODO(dwslim): this is probably bad!
        // we should somehow enforce next_ to be a member of the template parameter ShadowCell?
        // but it works for now :)

        free_list_ = free_list_->next_;
        // cur->Reset();
        return cur;
      }
      Refill<size2>();
    }
    return shadow_pool_cur_++;
  }

  void free(ShadowCell* shadow) {
    // Lock l(&shadow->mtx_);
    shadow->next_ = free_list_;
    free_list_ = shadow;
  }

  INode node;

 private:
  template <uptr size>
  void Refill() {
    // refills++;
    // Printf("ShadowAlloc Refill %u\n", refills);
    shadow_pool_cur_ = (ShadowCell*) MmapOrDie(size * sizeof(ShadowCell), "ShadowAlloc");
    shadow_pool_end_ = shadow_pool_cur_ + size;
  }

  ShadowCell* shadow_pool_cur_;
  ShadowCell* shadow_pool_end_;
  ShadowCell* free_list_;
  int refills = 0;
};

}  // namespace __psan

#endif  // PSAN_SHADOW_ALLOC_H
