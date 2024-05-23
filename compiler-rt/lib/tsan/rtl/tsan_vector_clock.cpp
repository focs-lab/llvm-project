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

#if TSAN_UCLOCK_MEASUREMENTS
#include "tsan_rtl.h"
#endif

namespace __tsan {

#if TSAN_VECTORIZE
const uptr kVectorClockSize = kThreadSlotCount * sizeof(Epoch) / sizeof(m128);
#endif

VectorClock::VectorClock() { Reset(); }

void VectorClock::Reset() {
#if TSAN_UCLOCKS
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = kEpochZero;
    uclk_[i] = kEpochZero;
  }

  // non threads must not have sid
  sid_ = kFreeSid;
  // only for syncs
  last_released_thread_ = kFreeSid;
  last_release_was_store_ = true;
#else
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = kEpochZero;
  }
#else
  m128 z = _mm_setzero_si128();
  m128* vclk = reinterpret_cast<m128*>(clk_);
  for (uptr i = 0; i < kVectorClockSize; i++) _mm_store_si128(&vclk[i], z);
#endif
#endif
}

#if TSAN_UCLOCK_MEASUREMENTS
void VectorClock::BBREAK() __attribute_noinline__ {
  Printf("BREAK!\n");
}
#endif

void VectorClock::Acquire(const VectorClock* src) {
  if (!src)
    return;

#if TSAN_UCLOCK_MEASUREMENTS
  // thr->num_original_acquires++;
  atomic_fetch_add(&ctx->num_original_acquires, 1, memory_order_relaxed);
#endif

#if TSAN_UCLOCKS
// Acq(t, l):
// 	If U_l(LR_l) <= U_t(LR_l):
// 		Return
// 	U_t := U_t join U_l
// 	If not (C_l ⊑ C_t):
// 		C_t := C_t join C_l
// 		U_t[t]++

  // Skip if the thread already knows what the lock knows
  // u_l ⊑ U_t
  // CHECK_EQ(src->sid_, kFreeSid);

  Sid last_released_thread_ = src->last_released_thread_;
  // bool can_skip = false;
  if (LIKELY(src->last_release_was_store_))
  if (LIKELY(src->GetUclk(last_released_thread_) <= GetUclk(last_released_thread_)))
  {
    // can_skip = true;
    return;
#if TSAN_UCLOCK_MEASUREMENTS
    // if skip, means the thread should know more than the sync
    for (uptr i = 0; i < kThreadSlotCount; ++i)
    {
      // if the sync still knows more than the thread, there is something wrong
      if (clk_[i] < src->clk_[i] || uclk_[i] < src->uclk_[i]) {
        BBREAK();
        can_skip = false;
      }
    }
#endif
  }
  // if (can_skip) return;

#if TSAN_UCLOCK_MEASUREMENTS
  // thr->num_uclock_acquires++;
  atomic_fetch_add(&ctx->num_uclock_acquires, 1, memory_order_relaxed);
#endif

  // Join as per normal (because checking C_l ⊑ C_t takes as much time as joining)
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
  if (LIKELY(did_acquire)) IncUclk();
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

#if TSAN_UCLOCKS
#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_original_releases, 1, memory_order_relaxed);
#endif
// Rel(t, l);
// 	If U_t(t) != U_l(t):
// 		C_l := C_t join C_l // Also equivalent to “C_l := C_t”. When using tree clocks, use the “MonotoneCopy” function; see TC paper
// 		U_l := U_t
// 		LR_l := t
// 	If (smp_t):
// 		U_t(t)++
// 		C_t(t)++
// 		smp_t := 0

  DCHECK_EQ(dst->sid_, kFreeSid);

  // Skip if no new information would be given to the lock
  // u_t ⊑ U_l
  // if (GetUclk(sid_) == dst->GetUclk(sid_)) return;

#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
#endif

  // Join as per normal
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    dst->clk_[i] = max(clk_[i], dst->clk_[i]);
    dst->uclk_[i] = max(uclk_[i], dst->uclk_[i]);
  }

  // The lock stores info about last released thread
  dst->last_released_thread_ = sid_;
  dst->last_release_was_store_ = false;

  // Increment the epochs
  // Only increment if any events sampled since last release
  // if (has_sampled_) {
  //   // C_t.inc, U_t.inc
  //   IncUclk();
  //   has_sampled_ = false;
  // }
#else
  dst->Acquire(this);
#endif
}

void VectorClock::ReleaseStore(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);

#if TSAN_UCLOCKS
#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_original_releases, 1, memory_order_relaxed);
#endif
// Rel(t, l);
// 	If U_t(t) != U_l(t):
// 		C_l := C_t join C_l // Also equivalent to “C_l := C_t”. When using tree clocks, use the “MonotoneCopy” function; see TC paper
// 		U_l := U_t
// 		LR_l := t
// 	If (smp_t):
// 		U_t(t)++
// 		C_t(t)++
// 		smp_t := 0

  DCHECK_EQ(dst->sid_, kFreeSid);

  // Skip if no new information would be given to the clock
  // u_t ⊑ U_l
  if (GetUclk(sid_) <= dst->GetUclk(sid_))
    return;

#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
#endif

  // Join instead of store
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    dst->clk_[i] = max(clk_[i], dst->clk_[i]);
    dst->uclk_[i] = max(uclk_[i], dst->uclk_[i]);
  }

  // The lock stores info about last released thread
  dst->last_released_thread_ = sid_;
  dst->last_release_was_store_ = true;

  // Increment the epochs
  // Only increment if any events sampled since last release
  // if (has_sampled_) {
  //   u16 u_t = static_cast<u16>(GetUclk(sid_));
  //   SetUclk(sid_, static_cast<Epoch>(u_t+1));
  //   u16 c_t = static_cast<u16>(Get(sid_));
  //   Set(sid_, static_cast<Epoch>(c_t+1));
  //   has_sampled_ = false;
  // }
#else
  *dst = *this;
#endif
}

#if TSAN_UCLOCKS
// Only for atomic_store with memory_order_release
void VectorClock::ReleaseStoreAtomic(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);

#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_original_releases, 1, memory_order_relaxed);
#endif

  // Atomic store with release order is used to synchronize between 2 threads
  // Must copy even if the releasing thread has not performed any updates
  // i.e. acquired any clocks or sampled any events
  // so that the acquiring thread can acquire this thread's clock
  // However we can apply the same skipping logic
  // if the last released thread is the same as the current one
  // u_t ⊑ U_l
  // Check first if the atomic variable was last released to by this thread
  // if (dst->last_released_thread_ == sid_ && GetUclk(sid_) <= dst->GetUclk(sid_))
  //   return;

#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
#endif

  // Vector clock copy on both clocks
  *dst = *this;

  // The lock stores info about last released thread
  dst->last_released_thread_ = sid_;
  dst->last_release_was_store_ = true;

  // Increment the epochs
  // Only increment if any events sampled since last release
  // if (has_sampled_) {
  //   u16 u_t = static_cast<u16>(GetUclk(sid_));
  //   SetUclk(sid_, static_cast<Epoch>(u_t+1));
  //   u16 c_t = static_cast<u16>(Get(sid_));
  //   Set(sid_, static_cast<Epoch>(c_t+1));
  //   has_sampled_ = false;
  // }

  IncUclk();
}

void VectorClock::ReleaseFork(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);
  *dst = *this;
  dst->last_release_was_store_ = true;

#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_original_releases, 1, memory_order_relaxed);
  atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
#endif
}

void VectorClock::AcquireFromFork(const VectorClock* src) {
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    // Also join the augmented clock
    clk_[i] = max(clk_[i], src->clk_[i]);
    uclk_[i] = max(uclk_[i], src->uclk_[i]);
  }
  IncUclk();
}

void VectorClock::AcquireJoin(const VectorClock* child) {
// Join(tp, tc):
// 	If U_tc(tc) <= U_tp(tc):
// 		Return
// 	U_tp := U_tp join U_tc
// 	If not (C_tc ⊑ C_tp):
// 		C_tp := C_tp join C_tc
// 		U_tp[tp]++
#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_original_acquires, 1, memory_order_relaxed);
#endif

  // Skip if the thread already knows what the lock knows
  // u_l ⊑ U_t
  Sid tc = child->sid_;
  if (child->GetUclk(tc) <= GetUclk(tc))
    return;

#if TSAN_UCLOCK_MEASUREMENTS
  // thr->num_uclock_acquires++;
  atomic_fetch_add(&ctx->num_uclock_acquires, 1, memory_order_relaxed);
#endif

  // Join as per normal (because checking C_l ⊑ C_t takes as much time as joining)
  bool did_acquire = false;
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    if (clk_[i] < child->clk_[i]) {
      clk_[i] = child->clk_[i];
      did_acquire = true;
    }
    // Also join the augmented clock
    uclk_[i] = max(uclk_[i], child->uclk_[i]);
  }

  // If learnt something new about the lock, increment augmented epoch to signal that future releases will give new information
  if (did_acquire) IncUclk();
}
#endif

VectorClock& VectorClock::operator=(const VectorClock& other) {
#if TSAN_UCLOCKS
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

// Only for golang so we don't care about this
void VectorClock::ReleaseStoreAcquire(VectorClock** dstp) {
  CHECK(0);
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
#if TSAN_UCLOCKS
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
