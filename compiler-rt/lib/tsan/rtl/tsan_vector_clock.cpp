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

#if TSAN_UCLOCK_MEASUREMENTS
#include "tsan_rtl.h"
#endif

namespace __tsan {

#if TSAN_VECTORIZE
const uptr kVectorClockSize = kThreadSlotCount * sizeof(Epoch) / sizeof(m128);
#endif

VectorClock::VectorClock() {
  Reset();
}

void VectorClock::Reset() {
#if TSAN_OL
  if (UNLIKELY(clock_)) clock_->DropRef();
  clock_ = New<SharedClock>();
  is_shared_ = false;

  // non threads must not have sid
  sid_ = kFreeSid;

  local_ = kEpochZero;
  acquired_sid_ = kFreeSid;
  acquired_ = kEpochZero;
#elif TSAN_UCLOCKS
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

#if TSAN_OL
__attribute_noinline__ void VectorClock::BBREAK() const {
  Printf("BREAK!\n");
}

static SyncClock* AllocSync(SyncClock** dstp) {
  if (UNLIKELY(!*dstp))
    *dstp = New<SyncClock>();
  return *dstp;
}

void VectorClock::Unshare() {
  SharedClock* clock = New<SharedClock>(clock_);

  clock_->DropRef();
  clock_ = clock;
  is_shared_ = false;

  // Copy the dirty epochs to the clock
  // clock_->Set(sid_, local_);
  // if (LIKELY(acquired_sid_ != kFreeSid))
  //   clock_->Set(acquired_sid_, acquired_);
  // IncU();
  // IncU();

  // Clear the dirty epochs
  acquired_sid_ = kFreeSid;
  acquired_ = kEpochZero;
}

void VectorClock::Acquire(const SyncClock* src) {
  if (!src || !src->clock()) return;
  // Printf("Acquire\n");

#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_acquires, 1, memory_order_relaxed);
#endif

    Sid last_released_thread = src->LastReleasedThread();
    Epoch u_l = src->u();
    Epoch u_t_lr = GetU(last_released_thread);
    s16 diff = static_cast<u16>(u_l) - static_cast<u16>(u_t_lr);
  if (LIKELY(src->LastReleaseWasStore())) {
    if (diff <= 0) return;
    if (UNLIKELY(diff > static_cast<s16>(kFreeSid))) {
      diff = static_cast<s16>(kFreeSid);
    }

    SetU(last_released_thread, u_l);

    Sid curr = src->clock()->head();
    Epoch curr_epoch;
    for (s16 i = 0; i < diff; ++i) {
#if TSAN_OL_MEASUREMENTS
      atomic_fetch_add(&ctx->num_acquire_traverses, 1, memory_order_relaxed);
#endif
      curr_epoch = src->clock()->Get(curr);
      // else if (UNLIKELY(curr == src->acquired_sid()))
      //   curr_epoch = src->acquired();

      if (curr_epoch > clock_->Get(curr)) {
        if (UNLIKELY(IsShared())) {
          // if (LIKELY(acquired_sid_ == kFreeSid)) {
          //   acquired_sid_ = curr;
          //   acquired_ = curr_epoch;
          //   curr = src->clock()->Next(curr);
          //   continue;
          // }
          // else if (curr == acquired_sid_) {
          //   if (curr_epoch > acquired_) acquired_ = curr_epoch;
          //   curr = src->clock()->Next(curr);
          //   continue;
          // }
          // else {
#if TSAN_OL_MEASUREMENTS
              atomic_fetch_add(&ctx->num_acquire_deep_copies, 1, memory_order_relaxed);
#endif
            Unshare();
          // }
        }
#if TSAN_OL_MEASUREMENTS
          atomic_fetch_add(&ctx->num_acquire_updates, 1, memory_order_relaxed);
#endif
        Set(curr, curr_epoch);
        IncU();
      }

      curr = src->clock()->Next(curr);
    }

    if (src->local() > Get(last_released_thread)) {
      if (UNLIKELY(IsShared())) {
#if TSAN_OL_MEASUREMENTS
        atomic_fetch_add(&ctx->num_acquire_deep_copies, 1, memory_order_relaxed);
#endif
        Unshare();
      }
      Set(last_released_thread, src->local());
      IncU();
    }
  }
  else {
    Epoch curr_epoch;
    for (s16 i = 0; i < static_cast<s16>(kThreadSlotCount)-1; ++i) {
      curr_epoch = src->clock()->Get(i);
      if (curr_epoch > clock_->Get(i)) {
        if (UNLIKELY(IsShared())) {
#if TSAN_OL_MEASUREMENTS
          atomic_fetch_add(&ctx->num_acquire_deep_copies, 1, memory_order_relaxed);
#endif
          Unshare();
        }
#if TSAN_OL_MEASUREMENTS
    atomic_fetch_add(&ctx->num_acquire_updates, 1, memory_order_relaxed);
#endif
        Set(static_cast<Sid>(i), curr_epoch);
        IncU();
      }
    }
  }

  for (uptr i = 0; i < kThreadSlotCount; ++i) {
    Epoch src_curr_epoch = src->clock()->Get(i);
    Epoch my_curr_epoch = clock_->Get(i);
    if (src->LastReleaseWasStore())
    if (static_cast<Sid>(i) == last_released_thread) src_curr_epoch = src->local();
    if (static_cast<Sid>(i) == sid_) my_curr_epoch = local_;
    if (my_curr_epoch < src_curr_epoch) {
      Printf("#%u acquire %u: GG! %u, %u:%u, u_l: %u, u_t: %u last_released_was_store: %u last_released_was_atomic: %u diff: %d\n", sid_, last_released_thread, i, my_curr_epoch, src_curr_epoch, static_cast<u16>(src->u()), u_t_lr, src->LastReleaseWasStore(),src->LastReleaseWasAtomic(), diff);
      Printf("%p vs %p, %d\n", clock_, src->clock(), static_cast<u16>(u_l) - static_cast<u16>(u_t_lr));
      // for (uptr j = 0; j < kThreadSlotCount; ++j) Printf("%u:%u ", clock_->Get(j), src->clock()->Get(j));
      Printf("\n");
      // Sid curr = src->clock()->head();
      // for (uptr j = 0; j < kThreadSlotCount-1; ++j) {
      //   Printf("%u ", curr);
      //   curr = src->clock()->Next(curr);
      // }
      Printf("\n");
      CHECK(0);
    }
  }
}

void VectorClock::AcquireFromFork(const SyncClock* src) {
  DCHECK_EQ(Get(sid_), 1);
  // CHECK_EQ(GetU(sid_), 1);
  DCHECK_EQ(clock_->head(), sid_);

  for (uptr i = 0; i < kThreadSlotCount; ++i) {
    if (LIKELY(static_cast<Sid>(i) != sid_)) {
      clock_->SetOnly(i, src->clock()->Get(i));
    }
    // IncU();
  }

  // Get the parent thread's u epoch.
  // SetU(src->LastReleasedThread(), src->u());
}

void VectorClock::AcquireJoin(const SyncClock* src) {
  Acquire(src);
}

void VectorClock::Release(SyncClock** dstp) {
  // Printf("#%u Release\n", sid_);
  SyncClock* dst = AllocSync(dstp);

  // if there is no clock to join with then just shallow copy
  if (UNLIKELY(!dst->clock())) {
    dst->SetClock(clock_);
    dst->SetU(GetU(sid_));

    dst->SetLocal(local_);
    dst->SetLastReleasedThread(sid_);
    dst->SetLastReleaseWasStore();
    is_shared_ = true;

    dst->SetAcquired(acquired_);
    dst->SetAcquiredSid(acquired_sid_);
  }
  else {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_release_joins, 1, memory_order_relaxed);
#endif
    // Dont actually need to allocate again if it's just this sync holding the shared clock
    if (UNLIKELY(!dst->clock()->IsShared())) dst->clock()->Join(clock_);
    else {
      SharedClock* joined_clock = New<SharedClock>(clock_, dst->clock());
      dst->SetClock(joined_clock);
      joined_clock->DropRef();  // drop a reference held by the "new" call

      // after doing deep copy, update the local epoch in the clock
      if (dst->local() > dst->clock()->Get(dst->LastReleasedThread())) dst->clock()->SetOnly(dst->LastReleasedThread(), dst->local());
    }

    // remember that the thread should release its local epoch to the sync too
    if (local_ > dst->clock()->Get(sid_)) dst->clock()->SetOnly(sid_, local_);
    dst->ClearLastReleaseWasStore();
  }

  // dst->ClearLastReleaseWasAtomic();
}

void VectorClock::ReleaseStore(SyncClock** dstp) {
  // Printf("#%u Release\n", sid_);
  SyncClock* dst = AllocSync(dstp);

  dst->SetClock(clock_);
  dst->SetU(GetU(sid_));

  dst->SetLocal(local_);
  dst->SetLastReleasedThread(sid_);
  dst->SetLastReleaseWasStore();
  is_shared_ = true;

  dst->SetAcquired(acquired_);
  dst->SetAcquiredSid(acquired_sid_);
  // dst->ClearLastReleaseWasAtomic();
}

void VectorClock::ReleaseStoreAtomic(SyncClock** dstp) {
  SyncClock* dst = AllocSync(dstp);

  dst->SetClock(clock_);
  dst->SetU(GetU(sid_));

  dst->SetLocal(local_);
  dst->SetLastReleasedThread(sid_);
  dst->SetLastReleaseWasStore();
  is_shared_ = true;

  dst->SetAcquired(acquired_);
  dst->SetAcquiredSid(acquired_sid_);
  // dst->SetLastReleaseWasAtomic();
}

void VectorClock::ReleaseFork(SyncClock** dstp) {
  ReleaseStore(dstp);
}

void VectorClock::ReleaseStoreAcquire(SyncClock** dstp) {
  CHECK(0);
}

void VectorClock::ReleaseAcquire(SyncClock** dstp) {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_release_acquires, 1, memory_order_relaxed);
#endif
  SyncClock* dst = AllocSync(dstp);
  Acquire(dst);
  ReleaseStore(dstp);
}
#else
void VectorClock::Acquire(const VectorClock* src) {
  if (!src)
    return;

#if TSAN_EMPTY
  return;
#endif

#if TSAN_UCLOCK_MEASUREMENTS
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
  if (UNLIKELY(!(src->GetU(last_released_thread_) <= GetU(last_released_thread_) && src->last_release_was_store_))) {

    // bool can_skip = false;
  //   if (LIKELY(src->last_release_was_store_))
  //   if (LIKELY(src->GetUclk(last_released_thread_) <= GetUclk(last_released_thread_)))
  //   {
  //     // can_skip = true;
  //     return;
  // #if TSAN_UCLOCK_MEASUREMENTS
  //     // if skip, means the thread should know more than the sync
  //     for (uptr i = 0; i < kThreadSlotCount; ++i)
  //     {
  //       // if the sync still knows more than the thread, there is something wrong
  //       if (clk_[i] < src->clk_[i] || uclk_[i] < src->uclk_[i]) {
  //         BBREAK();
  //         can_skip = false;
  //       }
  //     }
  // #endif
  //   }
    // if (can_skip) return;

#if TSAN_UCLOCK_MEASUREMENTS
    atomic_fetch_add(&ctx->num_uclock_acquires, 1, memory_order_relaxed);
#endif

    // Join as per normal (because checking C_l ⊑ C_t takes as much time as joining)
    bool did_acquire = false;
    for (uptr i = 0; i < kThreadSlotCount; i++) {
      uclk_[i] = max(uclk_[i], src->uclk_[i]);
      Epoch sc = src->clk_[i];
      clk_[i] = max(clk_[i], sc);
      did_acquire |= clk_[i] == sc;
    }

    // If learnt something new about the lock, increment augmented epoch to signal that future releases will give new information
    if (LIKELY(did_acquire)) IncU();
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
#if TSAN_EMPTY
  return;
#endif

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
  if (LIKELY(GetU(sid_) != dst->GetU(sid_))) {

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
  // Release is called by operations that do not necessarily acquire before release
  dst->last_release_was_store_ = false;
  }
#else
  dst->Acquire(this);
#endif
}

void VectorClock::ReleaseStore(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);
#if TSAN_EMPTY
  return;
#endif

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
  if (LIKELY(GetU(sid_) != dst->GetU(sid_))) {

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
  }
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
  if (dst->last_released_thread_ == sid_ && GetU(sid_) <= dst->GetU(sid_))
    return;

#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
#endif

  // Vector clock copy on both clocks
  *dst = *this;

  // The lock stores info about last released thread
  dst->last_released_thread_ = sid_;
  dst->last_release_was_store_ = true;
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
  // This is called after SlotAttachAndLock, which will increment the epoch of the child
  // Maybe doing max is overkill, but need to confirm whether doing copy affects correctness
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    // Also join the augmented clock
    // clk_[i] = max(clk_[i], src->clk_[i]);
    // uclk_[i] = max(uclk_[i], src->uclk_[i]);
    if (LIKELY(static_cast<Sid>(i) != sid_)) {
      clk_[i] = src->clk_[i];
      uclk_[i] = src->uclk_[i];
    }
  }
  // IncUclk();
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
  // if (LIKELY(child->GetUclk(tc) > GetUclk(tc))) {

#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_uclock_acquires, 1, memory_order_relaxed);
#endif

  // Join as per normal (because checking C_l ⊑ C_t takes as much time as joining)
  bool did_acquire = false;
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    uclk_[i] = max(uclk_[i], child->uclk_[i]);
    Epoch cc = child->clk_[i];
    clk_[i] = max(clk_[i], cc);
    did_acquire |= clk_[i] == cc;
  }

  // If learnt something new about the lock, increment augmented epoch to signal that future releases will give new information
  if (LIKELY(did_acquire)) IncU();
  // }
}
#endif

VectorClock& VectorClock::operator=(const VectorClock& other) {
#if TSAN_EMPTY
  return *this;
#endif
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
#if TSAN_EMPTY
  return;
#endif
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
#if TSAN_EMPTY
  return;
#endif
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
#endif

}  // namespace __tsan
