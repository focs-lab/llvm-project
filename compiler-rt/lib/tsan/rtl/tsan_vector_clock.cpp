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

namespace __tsan {

#if TSAN_VECTORIZE
const uptr kVectorClockSize = kThreadSlotCount * sizeof(Epoch) / sizeof(m128);
#endif

VectorClock::VectorClock() { Reset(); }

void VectorClock::Reset() {
#if TSAN_MINJIAN
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = kEpochZero;
    uclk_[i] = kEpochZero;
  }

  // only for locks
  last_released_ = kFreeSid;
  lock_uclk_ = kEpochZero;
  return;
#endif
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = kEpochZero;
  }
#else
  m128 z = _mm_setzero_si128();
  m128* vclk = reinterpret_cast<m128*>(clk_);
  for (uptr i = 0; i < kVectorClockSize; i++) _mm_store_si128(&vclk[i], z);
#endif
}

void VectorClock::Acquire(const VectorClock* src) {
  if (!src)
    return;
#if TSAN_MINJIAN
  // Skip if the lock's last released thread has already been acquired before
  // u_l ⊑ U_t
  if (src->lock_uclk_ <= GetUclk(src->last_released_))
    return;

  // Join as per normal
  bool did_acquire = false;
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    if (clk_[i] < src->clk_[i]) {
      clk_[i] = src->clk_[i];
      did_acquire = true;
    }
    // Also join the augmented clock
    uclk_[i] = max(uclk_[i], src->uclk_[i]);
  }

  // If learnt something new about the lock, increment augmented epoch to signal that future releases will give new information
  if (did_acquire) {
    u16 u_t = static_cast<u16>(GetUclk(sid_));
    SetUclk(sid_, static_cast<Epoch>(u_t + 1));
  }
#else
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++)
    clk_[i] = max(clk_[i], src->clk_[i]);
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
  m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(src->clk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    m128 s = _mm_load_si128(&vsrc[i]);
    m128 d = _mm_load_si128(&vdst[i]);
    m128 m = _mm_max_epu16(s, d);
    _mm_store_si128(&vdst[i], m);
  }
#endif
#endif
}

static VectorClock* AllocClock(VectorClock** dstp) {
  if (UNLIKELY(!*dstp))
    *dstp = New<VectorClock>();
  return *dstp;
}

void VectorClock::Release(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);

#if TSAN_MINJIAN
  // Increment the epochs
  // Only increment if any events sampled since last release
  if (has_sampled_) {
    // C_t.inc
    u16 c_t = static_cast<u16>(Get(sid_));
    Set(sid_, static_cast<Epoch>(c_t+1));
    // U_t.inc
    u16 u_t = static_cast<u16>(GetUclk(sid_));
    SetUclk(sid_, static_cast<Epoch>(u_t+1));
    has_sampled_ = false;
  }

  // Skip if no new information would be given to the clock
  // u_t ⊑ U_l
  if (GetUclk(sid_) <= dst->GetUclk(sid_))
    return;

  // Join as per normal
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    dst->clk_[i] = max(clk_[i], dst->clk_[i]);
    dst->uclk_[i] = max(uclk_[i], dst->uclk_[i]);
  }

  // The lock stores info about last released thread
  dst->lock_uclk_ = GetUclk(sid_);
  dst->last_released_ = sid_;
#else
  dst->Acquire(this);
#endif
}

void VectorClock::ReleaseStore(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);

#if TSAN_MINJIAN
  // Increment the epochs
  // Only increment if any events sampled since last release
  if (has_sampled_) {
    u16 u_t = static_cast<u16>(GetUclk(sid_));
    SetUclk(sid_, static_cast<Epoch>(u_t+1));
    u16 c_t = static_cast<u16>(Get(sid_));
    Set(sid_, static_cast<Epoch>(c_t+1));
    has_sampled_ = false;
  }

  // Skip if no new information would be given to the clock
  // u_t ⊑ U_l
  if (GetUclk(sid_) <= dst->GetUclk(sid_))
    return;

  // Copy as per normal
  *dst = *this;

  // The lock stores info about last released thread
  dst->lock_uclk_ = GetUclk(sid_);
  dst->last_released_ = sid_;
#else
  *dst = *this;
#endif
}

#if TSAN_MINJIAN
void VectorClock::ReleaseStoreAtomic(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);

  // Increment the epochs
  // Only increment if any events sampled since last release
  if (has_sampled_) {
    u16 u_t = static_cast<u16>(GetUclk(sid_));
    SetUclk(sid_, static_cast<Epoch>(u_t+1));
    u16 c_t = static_cast<u16>(Get(sid_));
    Set(sid_, static_cast<Epoch>(c_t+1));
    has_sampled_ = false;
  }

  // Atomic store with release order is used to synchronize between 2 threads
  // Must copy even if the releasing thread has not performed any updates
  // i.e. acquired any clocks or sampled any events
  // so that the acquiring thread can acquire this thread's clock
  // However we can apply the same skipping logic
  // if the last released thread is the same as the current one
  // u_t ⊑ U_l
  if (dst->last_released_ == sid_ && GetUclk(sid_) <= dst->GetUclk(sid_))
    return;

  // Copy as per normal
  *dst = *this;

  // The lock stores info about last released thread
  dst->lock_uclk_ = GetUclk(sid_);
  dst->last_released_ = sid_;
}
#endif

VectorClock& VectorClock::operator=(const VectorClock& other) {
#if TSAN_MINJIAN
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = other.clk_[i];
    uclk_[i] = other.uclk_[i];
  }
#else
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++)
    clk_[i] = other.clk_[i];
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
  m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(other.clk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    m128 s = _mm_load_si128(&vsrc[i]);
    _mm_store_si128(&vdst[i], s);
  }
#endif
#endif
  return *this;
}

void VectorClock::ReleaseStoreAcquire(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    Epoch tmp = dst->clk_[i];
    dst->clk_[i] = clk_[i];
    clk_[i] = max(clk_[i], tmp);
  }
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(dst->clk_);
  m128* __restrict vclk = reinterpret_cast<m128*>(clk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    m128 t = _mm_load_si128(&vdst[i]);
    m128 c = _mm_load_si128(&vclk[i]);
    m128 m = _mm_max_epu16(c, t);
    _mm_store_si128(&vdst[i], c);
    _mm_store_si128(&vclk[i], m);
  }
#endif
}

void VectorClock::ReleaseAcquire(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);
#if TSAN_MINJIAN
  Acquire(dst);
  ReleaseStore(dstp);
#else
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    dst->clk_[i] = max(dst->clk_[i], clk_[i]);
    clk_[i] = dst->clk_[i];
  }
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(dst->clk_);
  m128* __restrict vclk = reinterpret_cast<m128*>(clk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    m128 c = _mm_load_si128(&vclk[i]);
    m128 d = _mm_load_si128(&vdst[i]);
    m128 m = _mm_max_epu16(c, d);
    _mm_store_si128(&vdst[i], m);
    _mm_store_si128(&vclk[i], m);
  }
#endif
#endif
}

}  // namespace __tsan
