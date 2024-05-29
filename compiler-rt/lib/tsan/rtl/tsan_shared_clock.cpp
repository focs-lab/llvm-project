//===-- tsan_vector_clock.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
//===----------------------------------------------------------------------===//
#include "tsan_vector_clock.h"

#include "sanitizer_common/sanitizer_placement_new.h"
#include "tsan_mman.h"
#include "tsan_rtl.h"

namespace __tsan {
#if TSAN_OL_MEASUREMENTS
void SharedClock::HoldRef() {
  atomic_fetch_add(&ctx->num_holds, 1, memory_order_relaxed);
  atomic_fetch_add(&ref_cnt, 1, memory_order_relaxed);
}

void SharedClock::DropRef() {
  DCHECK_GT(atomic_load_relaxed(&ref_cnt), 0);
  atomic_fetch_add(&ctx->num_drops, 1, memory_order_relaxed);
  if (atomic_load_relaxed(&ref_cnt) == 1) {
    atomic_fetch_add(&ctx->num_frees, 1, memory_order_relaxed);
    FreeImpl(this);
    return;
  }
  atomic_fetch_sub(&ref_cnt, 1, memory_order_relaxed);
}
#endif

SharedClock::SharedClock() {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_deep_copies, 1, memory_order_relaxed);
#endif
  atomic_store_relaxed(&ref_cnt, 1);
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = kEpochZero;
    next_[i] = static_cast<Sid>(i+1);
    prev_[i] = static_cast<Sid>(i-1);
  }
  head_ = static_cast<Sid>(0);
}

SharedClock::SharedClock(const SharedClock* clock) {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_deep_copies, 1, memory_order_relaxed);
#endif
  atomic_store_relaxed(&ref_cnt, 1);
  *this = *clock;
}

SharedClock::SharedClock(const SharedClock* clock_t, const SharedClock* clock_l) {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_deep_copies, 1, memory_order_relaxed);
#endif
  // CHECK(clock_l);
  atomic_store_relaxed(&ref_cnt, 1);
  *this = *clock_l;
  Join(clock_t);
}

void SharedClock::Join(const SharedClock* other) {
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    Epoch cti = other->clk_[i];
    if (clk_[i] < cti) {
      Set(i, cti);
    }
  }
}

SharedClock& SharedClock::operator=(const SharedClock& other) {
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = other.clk_[i];
    next_[i] = other.next_[i];
    prev_[i] = other.prev_[i];
  }
  head_ = other.head_;

  return *this;
}

}