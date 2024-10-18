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
// #include <time.h>


namespace __tsan {

#if TSAN_VECTORIZE
const uptr kVectorClockSize = kThreadSlotCount * sizeof(Epoch) / sizeof(m128);
#if TSAN_OL
const uptr kOrderedListNodeSize = kThreadSlotCount * sizeof(Sid) / sizeof(m128);
#endif
#endif

VectorClock::VectorClock() {
#if TSAN_OL
  // We cannot assume that the contents are zeroed at the start.
  // We don't want Reset to see an uninitialized clock_ pointer.
  clock_ = nullptr;
#endif
  Reset();
}

void VectorClock::Reset() {
#if TSAN_OL
  if (UNLIKELY(clock_)) clock_->DropRef(alloc_);
  clock_ = alloc_.Make();
  is_shared_ = false;

#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    uclk_[i] = kEpochZero;
  }
#else
  m128 z = _mm_setzero_si128();
  m128* vuclk = reinterpret_cast<m128*>(uclk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    _mm_store_si128(&vuclk[i], z);
  }
#endif

  // non threads must not have sid
  sid_ = kFreeSid;
  sampled_ = false;

  local_ = kEpochZero;
  // acquired_sid_ = kFreeSid;
  // acquired_ = kEpochZero;
#elif TSAN_UCLOCKS
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = kEpochZero;
    uclk_[i] = kEpochZero;
  }
#else
  m128 z = _mm_setzero_si128();
  m128* vclk = reinterpret_cast<m128*>(clk_);
  m128* vuclk = reinterpret_cast<m128*>(uclk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    _mm_store_si128(&vclk[i], z);
    _mm_store_si128(&vuclk[i], z);
  }
#endif

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

VectorClock::~VectorClock() {
  DCHECK(clock_);
  clock_->DropRef(alloc_);
}

static SyncClock* AllocSync(SyncClock** dstp) {
  if (UNLIKELY(!*dstp))
    *dstp = New<SyncClock>();
  return *dstp;
}

void VectorClock::Unshare() {
  SharedClock* clock = alloc_.Make(clock_);

  clock_->DropRef(alloc_);
  clock_ = clock;
  is_shared_ = false;

  // Copy the dirty epochs to the clock
  // clock_->Set(sid_, local_for_release());
  // if (LIKELY(acquired_sid_ != kFreeSid))
  //   clock_->Set(acquired_sid_, acquired_);
  // IncU();
  // IncU();

  // Clear the dirty epochs
  // acquired_sid_ = kFreeSid;
  // acquired_ = kEpochZero;
}

void VectorClock::Acquire(const SyncClock* src) {
  if (!src || !src->clock()) return;
  // Printf("Acquire\n");

  #if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_acquires, 1, memory_order_relaxed);
  #endif

  if (LIKELY(src->LastReleaseWasStore())) {
    Sid last_released_thread = src->LastReleasedThread();
    if (last_released_thread == sid_) return;

    // Update based on dirty epoch
    // Do it at the start to prevent accidentally early returning due to things like diff == 0
    if (src->local() > Get(last_released_thread)) {
      if (UNLIKELY(IsShared())) {
        #if TSAN_OL_MEASUREMENTS
        atomic_fetch_add(&ctx->num_acquire_deep_copies, 1, memory_order_relaxed);
        #endif
        Unshare();
      }
      #if TSAN_OL_MEASUREMENTS
      atomic_fetch_add(&ctx->num_acquire_ll_updates, 1, memory_order_relaxed);
      #endif
      Set(last_released_thread, src->local());
      IncU();
      #if TSAN_OL_MEASUREMENTS
      u64 max_u = atomic_load_relaxed(&ctx->max_u);
      while (!atomic_compare_exchange_strong(&ctx->max_u, &max_u, max(max_u, static_cast<u64>(GetU(sid_))), memory_order_relaxed));
      #endif
    }

    Epoch u_l = src->u();
    Epoch u_t_lr = GetU(last_released_thread);
    s32 diff = static_cast<s32>(u_l) - static_cast<s32>(u_t_lr);
    if (diff <= 0) return;
    if (UNLIKELY(diff > static_cast<s32>(kFreeSid))) {
      diff = static_cast<s32>(kFreeSid);
    }

    SetU(last_released_thread, u_l);

    Sid curr = src->clock()->head();
    Epoch curr_epoch;
    for (s32 i = 0; i < diff; ++i) {
      #if TSAN_OL_MEASUREMENTS
      atomic_fetch_add(&ctx->num_acquire_ll_traverses, 1, memory_order_relaxed);
      #endif
      if (UNLIKELY(curr == last_released_thread)) {
        curr = src->clock()->Next(curr);
        continue;
      }
      curr_epoch = src->clock()->Get(curr);

      if (curr_epoch > clock_->Get(curr)) {
        if (UNLIKELY(IsShared())) {
          #if TSAN_OL_MEASUREMENTS
          atomic_fetch_add(&ctx->num_acquire_deep_copies, 1, memory_order_relaxed);
          #endif
          Unshare();
        }
        #if TSAN_OL_MEASUREMENTS
        atomic_fetch_add(&ctx->num_acquire_ll_updates, 1, memory_order_relaxed);
        #endif
        Set(curr, curr_epoch);
        IncU();
      }

      curr = src->clock()->Next(curr);
    }
  }
  else {
    Epoch curr_epoch;
    // kThreadSlotCount-1 because we ignore the kFreeSid slot
    for (uptr i = 0; i < kThreadSlotCount-1; ++i) {
      #if TSAN_OL_MEASUREMENTS
      atomic_fetch_add(&ctx->num_acquire_arr_traverses, 1, memory_order_relaxed);
      #endif
      curr_epoch = src->clock()->Get(i);
      if (curr_epoch > clock_->Get(i)) {
        if (UNLIKELY(IsShared())) {
          #if TSAN_OL_MEASUREMENTS
          atomic_fetch_add(&ctx->num_acquire_deep_copies, 1, memory_order_relaxed);
          #endif
          Unshare();
        }
        #if TSAN_OL_MEASUREMENTS
        atomic_fetch_add(&ctx->num_acquire_ll_updates, 1, memory_order_relaxed);
        #endif
        Set(static_cast<Sid>(i), curr_epoch);
        IncU();
      }
    }
  }

#if TSAN_OL_MEASUREMENTS
  u64 max_u = atomic_load_relaxed(&ctx->max_u);
  while (!atomic_compare_exchange_strong(&ctx->max_u, &max_u, max(max_u, static_cast<u64>(GetU(sid_))), memory_order_relaxed));
#endif

  // for (uptr i = 0; i < kThreadSlotCount; ++i) {
  //   Epoch src_curr_epoch = src->clock()->Get(i);
  //   Epoch my_curr_epoch = clock_->Get(i);
  //   if (src->LastReleaseWasStore())
  //   if (static_cast<Sid>(i) == last_released_thread) src_curr_epoch = src->local();
  //   if (static_cast<Sid>(i) == sid_) my_curr_epoch = local_for_release();
  //   if (my_curr_epoch < src_curr_epoch) {
  //     Printf("#%u acquire %u: GG! %u, %u:%u, u_l: %u, u_t: %u last_released_was_store: %u last_released_was_atomic: %u diff: %d\n", sid_, last_released_thread, i, my_curr_epoch, src_curr_epoch, static_cast<u16>(src->u()), u_t_lr, src->LastReleaseWasStore(),src->LastReleaseWasAtomic(), diff);
  //     Printf("%p vs %p, %d\n", clock_, src->clock(), static_cast<u16>(u_l) - static_cast<u16>(u_t_lr));
  //     // for (uptr j = 0; j < kThreadSlotCount; ++j) Printf("%u:%u ", clock_->Get(j), src->clock()->Get(j));
  //     Printf("\n");
  //     // Sid curr = src->clock()->head();
  //     // for (uptr j = 0; j < kThreadSlotCount-1; ++j) {
  //     //   Printf("%u ", curr);
  //     //   curr = src->clock()->Next(curr);
  //     // }
  //     Printf("\n");
  //     CHECK(0);
  //   }
  // }
}

void VectorClock::AcquireFromFork(const SyncClock* src) {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_acquires, 1, memory_order_relaxed);
#endif
  DCHECK_EQ(Get(sid_), 1);
  // CHECK_EQ(GetU(sid_), 1);
  DCHECK_EQ(clock_->head(), sid_);

  for (uptr i = 0; i < kThreadSlotCount; ++i) {
#if TSAN_OL_MEASUREMENTS
    atomic_fetch_add(&ctx->num_acquire_arr_traverses, 1, memory_order_relaxed);
    atomic_fetch_add(&ctx->num_acquire_arr_updates, 1, memory_order_relaxed);
#endif
    if (UNLIKELY(static_cast<Sid>(i) == src->LastReleasedThread()))
      clock_->SetOnly(i, src->local());
    else if (LIKELY(static_cast<Sid>(i) != sid_))
      clock_->SetOnly(i, src->clock()->Get(i));

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
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_releases, 1, memory_order_relaxed);
#endif

  // if there is no clock to join with then just shallow copy
  if (UNLIKELY(!dst->clock())) {
#if TSAN_OL_MEASUREMENTS
    atomic_fetch_add(&ctx->num_release_shallow_copies, 1, memory_order_relaxed);
#endif
    dst->SetClock(clock_, alloc_);
    dst->SetU(GetU(sid_));

    dst->SetLocal(local_for_release());
    dst->SetLastReleasedThread(sid_);
    dst->SetLastReleaseWasStore();
    is_shared_ = true;

    // dst->SetAcquired(acquired_);
    // dst->SetAcquiredSid(acquired_sid_);
  }
  else {
#if TSAN_OL_MEASUREMENTS
    atomic_fetch_add(&ctx->num_release_joins, 1, memory_order_relaxed);
#endif
    // Dont actually need to allocate again if it's just this sync holding the shared clock
    if (UNLIKELY(!dst->clock()->IsShared())) dst->clock()->Join(clock_);
    else {
#if TSAN_OL_MEASUREMENTS
      atomic_fetch_add(&ctx->num_release_deep_copies, 1, memory_order_relaxed);
#endif
      SharedClock* joined_clock = alloc_.Make(clock_, dst->clock());
      dst->SetClock(joined_clock, alloc_);
      joined_clock->DropRef(alloc_);  // drop a reference held by the "new" call

      // after doing deep copy, update the local epoch in the clock
#if TSAN_OL_MEASUREMENTS
      atomic_fetch_add(&ctx->num_release_arr_traverses, 2, memory_order_relaxed);
#endif
      if (dst->local() > dst->clock()->Get(dst->LastReleasedThread())) {
#if TSAN_OL_MEASUREMENTS
        atomic_fetch_add(&ctx->num_release_arr_updates, 1, memory_order_relaxed);
#endif
        dst->clock()->SetOnly(dst->LastReleasedThread(), dst->local());
      }
    }

    // remember that the thread should release its local epoch to the sync too
#if TSAN_OL_MEASUREMENTS
    atomic_fetch_add(&ctx->num_release_arr_traverses, 2, memory_order_relaxed);
#endif
    if (local_for_release() > dst->clock()->Get(sid_)) {
#if TSAN_OL_MEASUREMENTS
      atomic_fetch_add(&ctx->num_release_arr_updates, 1, memory_order_relaxed);
#endif
      dst->clock()->SetOnly(sid_, local_for_release());
    }
    dst->ClearLastReleaseWasStore();
  }

  // dst->ClearLastReleaseWasAtomic();
}

void VectorClock::ReleaseStore(SyncClock** dstp) {
  // Printf("#%u Release\n", sid_);
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_releases, 1, memory_order_relaxed);
  atomic_fetch_add(&ctx->num_release_shallow_copies, 1, memory_order_relaxed);
#endif
  SyncClock* dst = AllocSync(dstp);

  dst->SetClock(clock_, alloc_);
  dst->SetU(GetU(sid_));

  dst->SetLocal(local_for_release());
  dst->SetLastReleasedThread(sid_);
  dst->SetLastReleaseWasStore();
  is_shared_ = true;

  // dst->SetAcquired(acquired_);
  // dst->SetAcquiredSid(acquired_sid_);
  // dst->ClearLastReleaseWasAtomic();
}

// ALWAYS_INLINE void SyncClock::CopyClock(SharedClock* clock, Sid sid, Epoch u) {
//   if (clock_ && clock_->IsShared()) {
//     clock_->DropRef();
// #if TSAN_OL_MEASUREMENTS
//     atomic_fetch_add(&ctx->num_release_deep_copies, 1, memory_order_relaxed);
// #endif
//     clock_ = alloc_.Make(clock);
//   }
//   else if (!clock_) {
// #if TSAN_OL_MEASUREMENTS
//     atomic_fetch_add(&ctx->num_release_deep_copies, 1, memory_order_relaxed);
// #endif
//     clock_ = alloc_.Make(clock);
//   }
//   else if (last_released_thread_ == sid && static_cast<u16>(u) - static_cast<u16>(u_) <= 16) {
//     Epoch cs[16];
//     Sid sids[16];
//     u16 du = static_cast<u16>(u) - static_cast<u16>(u_);
//     Sid curr = clock->head();
//     for (u8 i = 0; i < du; ++i) {
// #if TSAN_OL_MEASUREMENTS
//       atomic_fetch_add(&ctx->num_release_ll_traverses, 1, memory_order_relaxed);
// #endif
//       Epoch curr_epoch = clock->Get(curr);
//       cs[i] = curr_epoch;
//       sids[i] = curr;
//       curr = clock->Next(curr);
//     }
//     for (s8 i = du-1; i >= 0; --i) {
// #if TSAN_OL_MEASUREMENTS
//       atomic_fetch_add(&ctx->num_release_ll_updates, 1, memory_order_relaxed);
// #endif
//       clock_->Set(sids[i], cs[i]);
//     }
//   }
//   else {
// #if TSAN_OL_MEASUREMENTS
//     atomic_fetch_add(&ctx->num_release_deep_copies, 1, memory_order_relaxed);
// #endif
//     *clock_ = *clock;
//   }
// }

void VectorClock::ReleaseStoreAtomic(SyncClock** dstp) {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_atomic_store_releases, 1, memory_order_relaxed);
  atomic_fetch_add(&ctx->num_release_shallow_copies, 1, memory_order_relaxed);
#endif
  SyncClock* dst = AllocSync(dstp);

  Epoch u = GetU(sid_);
  // dst->CopyClock(clock_, sid_, u);
  dst->SetClock(clock_, alloc_);
  dst->SetU(u);

  dst->SetLocal(local_for_release());
  dst->SetLastReleasedThread(sid_);
  dst->SetLastReleaseWasStore();

  is_shared_ = true;

  // dst->SetAcquired(acquired_);
  // dst->SetAcquiredSid(acquired_sid_);
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
  ReleaseStoreAtomic(dstp);
}
#else
// Please forgive me for writing code like this.
#if TSAN_UCLOCKS
void VectorClock::Acquire(VectorClock* src) {
#else
void VectorClock::Acquire(const VectorClock* src) {
#endif
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

  src->last_acquired_thread_ = sid_;

  // Skip if the thread already knows what the lock knows
  // u_l ⊑ U_t
  // CHECK_EQ(src->sid_, kFreeSid);

  Sid src_last_released_thread = src->last_released_thread_;

  // we will do the acquire if last release wasnt store (barriers), or the sync knows more than this thread
  if (UNLIKELY(!src->last_release_was_store_ || (src->GetU(src_last_released_thread) > GetU(src_last_released_thread)))) {
#if TSAN_UCLOCK_MEASUREMENTS
    atomic_fetch_add(&ctx->num_uclock_acquires, 1, memory_order_relaxed);
#endif

    // Join as per normal (because checking C_l ⊑ C_t takes as much time as joining)
    bool did_acquire = false;
#if !TSAN_VECTORIZE
    for (uptr i = 0; i < kThreadSlotCount; i++) {
      uclk_[i] = max(uclk_[i], src->uclk_[i]);
      Epoch sc = src->clk_[i];
      Epoch dc = clk_[i];
      clk_[i] = max(sc, dc);
      did_acquire |= sc > dc;
    }
#else
    m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
    m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(src->clk_);
    m128* __restrict vudst = reinterpret_cast<m128*>(uclk_);
    m128 const* __restrict vusrc = reinterpret_cast<m128 const*>(src->uclk_);
    for (uptr i = 0; i < kVectorClockSize; i++) {
      m128 s = _mm_load_si128(&vsrc[i]);
      m128 d = _mm_load_si128(&vdst[i]);
      m128 m = _mm_max_epu16(s, d);
      _mm_store_si128(&vdst[i], m);

      m128 su = _mm_load_si128(&vusrc[i]);
      m128 du = _mm_load_si128(&vudst[i]);
      m128 mu = _mm_max_epu16(su, du);
      _mm_store_si128(&vudst[i], mu);

      // test whether any entry has changed
      // if all are the same, then diff will be all zeros, and that means we didnt acquire anything
      // in other words, if not all zeros, means we acquired something
      m128 diff = _mm_xor_si128(d, m);
      did_acquire |= !_mm_test_all_zeros(diff, diff);
    }
#endif

    // If learnt something new about the lock, increment augmented epoch to signal that future releases will give new information
    if (LIKELY(did_acquire)) IncU();
  }
  // else {
  //   // Verify that it is correct. If we skip the acquire, we should truly know about everything the lock knows.
  //   for (uptr i = 0; i < kThreadSlotCount; ++i) {
  //     CHECK_GE(clk_[i], src->clk_[i]);
  //   }
  // }
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

  DCHECK_NE(sid_, kFreeSid);
  DCHECK_EQ(dst->sid_, kFreeSid);

  // Skip if no new information would be given to the lock
  // u_t ⊑ U_l
  // This is the negative case. There is new information to give to the sync.
  if (LIKELY(GetU(sid_) != dst->GetU(sid_))) {
#if TSAN_UCLOCK_MEASUREMENTS
    atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
#endif

    // Just make sure things are correct. The sync cannot know more than
    // the thread about itself.
    DCHECK_GE(clk_[static_cast<u8>(sid_)], dst->clk_[static_cast<u8>(sid_)]);

    // Join as per normal
#if !TSAN_VECTORIZE
    for (uptr i = 0; i < kThreadSlotCount; i++) {
      dst->clk_[i] = max(clk_[i], dst->clk_[i]);
      dst->uclk_[i] = max(uclk_[i], dst->uclk_[i]);
    }
#else
    m128* __restrict vdst = reinterpret_cast<m128*>(dst->clk_);
    m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(clk_);
    m128* __restrict vudst = reinterpret_cast<m128*>(dst->uclk_);
    m128 const* __restrict vusrc = reinterpret_cast<m128 const*>(uclk_);
    for (uptr i = 0; i < kVectorClockSize; i++) {
      m128 s = _mm_load_si128(&vsrc[i]);
      m128 d = _mm_load_si128(&vdst[i]);
      m128 m = _mm_max_epu16(s, d);
      _mm_store_si128(&vdst[i], m);

      m128 su = _mm_load_si128(&vusrc[i]);
      m128 du = _mm_load_si128(&vudst[i]);
      m128 mu = _mm_max_epu16(su, du);
      _mm_store_si128(&vudst[i], mu);
    }
#endif

    // Don't need to do max, it wouldnt make sense for the lock to know more about the thread than itself
    dst->SetLocal(sid_, local_for_release(), sampled_);

    // Since this thread has just sampled, the next thread that acquires from this
    // sync needs to know that there was an update by this thread.
    if (UNLIKELY(sampled_)) dst->SetU(sid_, IncU());

    // The lock stores info about last released thread
    dst->last_released_thread_ = sid_;
    // Release is called by operations that do not necessarily acquire before release
    dst->last_release_was_store_ = false;
    dst->last_acquired_thread_ = kFreeSid;
  }
  else if (UNLIKELY(sampled_)) {
    // If the sync knows everything about the thread, but thread just sampled.
    dst->Set(sid_, local_for_release());
    dst->SetU(sid_, IncU());
    dst->last_released_thread_ = sid_;
    dst->last_release_was_store_ = false;
    dst->last_acquired_thread_ = kFreeSid;
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

  DCHECK_NE(sid_, kFreeSid);
  DCHECK_EQ(dst->sid_, kFreeSid);

  // Skip if no new information would be given to the sync.
  // u_t ⊑ U_l
  // This is the negative case. There is new information to give to the sync.
  if (LIKELY(GetU(sid_) != dst->GetU(sid_))) {
#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
#endif

    // Just make sure things are correct. The sync cannot know more than
    // the thread about itself.
    DCHECK_GE(clk_[static_cast<u8>(sid_)], dst->clk_[static_cast<u8>(sid_)]);

    // Join instead of store
#if !TSAN_VECTORIZE
    for (uptr i = 0; i < kThreadSlotCount; i++) {
      dst->clk_[i] = max(clk_[i], dst->clk_[i]);
      dst->uclk_[i] = max(uclk_[i], dst->uclk_[i]);
    }
#else
    m128* __restrict vdst = reinterpret_cast<m128*>(dst->clk_);
    m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(clk_);
    m128* __restrict vudst = reinterpret_cast<m128*>(dst->uclk_);
    m128 const* __restrict vusrc = reinterpret_cast<m128 const*>(uclk_);
    for (uptr i = 0; i < kVectorClockSize; i++) {
      m128 s = _mm_load_si128(&vsrc[i]);
      m128 d = _mm_load_si128(&vdst[i]);
      m128 m = _mm_max_epu16(s, d);
      _mm_store_si128(&vdst[i], m);

      m128 su = _mm_load_si128(&vusrc[i]);
      m128 du = _mm_load_si128(&vudst[i]);
      m128 mu = _mm_max_epu16(su, du);
      _mm_store_si128(&vudst[i], mu);
    }
#endif

    // dont need to do max, it wouldnt make sense for the lock to know more about the thread than itself
    dst->SetLocal(sid_, local_for_release(), sampled_);
    // Since this thread has just sampled, the next thread that acquires from this
    // sync needs to know that there was an update by this thread.
    if (UNLIKELY(sampled_)) dst->SetU(sid_, IncU());

    // The lock stores info about last released thread
    dst->last_released_thread_ = sid_;
    dst->last_release_was_store_ = true;
    dst->last_acquired_thread_ = kFreeSid;
  }
  else if (UNLIKELY(sampled_)) {
    // If the sync knows everything about the thread, but thread just sampled.
    dst->Set(sid_, local_for_release());
    dst->SetU(sid_, IncU());
    dst->last_released_thread_ = sid_;
    dst->last_release_was_store_ = true;
    dst->last_acquired_thread_ = kFreeSid;
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

  // If this was the thread that last acquired, it means that this atomic variable
  // behaves like a mutex and grows monotonically, so our optimization can be applied
  // as per ReleaseStore.
  // Code is cleaner if we just call ReleaseStore, but I don't really want to have
  // additional function calls. Clean this up later.
  if (LIKELY(dst->last_acquired_thread_ == sid_)) {
    // Skip if no new information would be given to the sync.
    // u_t ⊑ U_l
    // This is the negative case. There is new information to give to the sync.
    if (LIKELY(GetU(sid_) != dst->GetU(sid_))) {
      #if TSAN_UCLOCK_MEASUREMENTS
      atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
      #endif

      // Just make sure things are correct. The sync cannot know more than
      // the thread about itself.
      DCHECK_GE(clk_[static_cast<u8>(sid_)], dst->clk_[static_cast<u8>(sid_)]);

      // Join instead of store
      #if !TSAN_VECTORIZE
      for (uptr i = 0; i < kThreadSlotCount; i++) {
        dst->clk_[i] = max(clk_[i], dst->clk_[i]);
        dst->uclk_[i] = max(uclk_[i], dst->uclk_[i]);
      }
      #else
      m128* __restrict vdst = reinterpret_cast<m128*>(dst->clk_);
      m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(clk_);
      m128* __restrict vudst = reinterpret_cast<m128*>(dst->uclk_);
      m128 const* __restrict vusrc = reinterpret_cast<m128 const*>(uclk_);
      for (uptr i = 0; i < kVectorClockSize; i++) {
        m128 s = _mm_load_si128(&vsrc[i]);
        m128 d = _mm_load_si128(&vdst[i]);
        m128 m = _mm_max_epu16(s, d);
        _mm_store_si128(&vdst[i], m);

        m128 su = _mm_load_si128(&vusrc[i]);
        m128 du = _mm_load_si128(&vudst[i]);
        m128 mu = _mm_max_epu16(su, du);
        _mm_store_si128(&vudst[i], mu);
      }
      #endif

      // dont need to do max, it wouldnt make sense for the lock to know more about the thread than itself
      dst->SetLocal(sid_, local_for_release(), sampled_);
      // Since this thread has just sampled, the next thread that acquires from this
      // sync needs to know that there was an update by this thread.
      if (UNLIKELY(sampled_)) dst->SetU(sid_, IncU());

      // The lock stores info about last released thread
      dst->last_released_thread_ = sid_;
      dst->last_release_was_store_ = true;
      dst->last_acquired_thread_ = kFreeSid;
    }
    else if (UNLIKELY(sampled_)) {
      // If the sync knows everything about the thread, but thread just sampled.
      dst->Set(sid_, local_for_release());
      dst->SetU(sid_, IncU());
      dst->last_released_thread_ = sid_;
      dst->last_release_was_store_ = true;
      dst->last_acquired_thread_ = kFreeSid;
    }
  }

  // Atomic release-store is used to synchronize between 2 threads
  // Must copy even if the releasing thread has not performed any updates
  // i.e. acquired any clocks or sampled any events
  // so that the acquiring thread can acquire this thread's clock
  // However we can apply the same skipping logic
  // if the last released thread is the same as the current one
  // u_t ⊑ U_l
  // Check first if the atomic variable was last released to by this thread,
  // or otherwise if the lock doesn't know about this thread.
  // This is the negative case. There is new information to give to the sync.
  else if (dst->last_released_thread_ != sid_ || GetU(sid_) > dst->GetU(sid_)) {
#if TSAN_UCLOCK_MEASUREMENTS
    atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
#endif
    DCHECK_NE(sid_, kFreeSid);

    // Vector clock copy on both clocks
    *dst = *this;
    dst->SetLocal(sid_, local_for_release(), sampled_);
    // Since this thread has just sampled, the next thread that acquires from this
    // sync needs to know that there was an update by this thread.
    if (UNLIKELY(sampled_)) dst->SetU(sid_, IncU());

    // The lock stores info about last released thread
    dst->last_released_thread_ = sid_;
    dst->last_release_was_store_ = true;
    dst->last_acquired_thread_ = kFreeSid;
  }
  else if (UNLIKELY(sampled_)) {
    // If the sync knows everything about the thread, but thread just sampled.
    dst->Set(sid_, local_for_release());
    dst->SetU(sid_, IncU());
    dst->last_released_thread_ = sid_;
    dst->last_release_was_store_ = true;
    dst->last_acquired_thread_ = kFreeSid;
  }
}

void VectorClock::ReleaseFork(VectorClock** dstp) {
  VectorClock* dst = AllocClock(dstp);
  *dst = *this;
  dst->SetLocal(sid_, local_for_release(), sampled_);
  if (UNLIKELY(sampled_)) dst->SetU(sid_, IncU());
  dst->last_release_was_store_ = true;
  dst->last_released_thread_ = sid_;
  dst->last_acquired_thread_ = kFreeSid;

#if TSAN_UCLOCK_MEASUREMENTS
  atomic_fetch_add(&ctx->num_original_releases, 1, memory_order_relaxed);
  atomic_fetch_add(&ctx->num_uclock_releases, 1, memory_order_relaxed);
#endif
}

void VectorClock::AcquireFromFork(const VectorClock* src) {
  // This is called after SlotAttachAndLock, which will increment the epoch of the child
  // Maybe doing max is overkill, but need to confirm whether doing copy affects correctness
  Epoch my_c = Get(sid_), my_u = GetU(sid_);
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    // Also join the augmented clock
    // clk_[i] = max(clk_[i], src->clk_[i]);
    // uclk_[i] = max(uclk_[i], src->uclk_[i]);
    clk_[i] = src->clk_[i];
    uclk_[i] = src->uclk_[i];
  }
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
  m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(src->clk_);
  m128* __restrict vudst = reinterpret_cast<m128*>(uclk_);
  m128 const* __restrict vusrc = reinterpret_cast<m128 const*>(src->clk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    m128 s = _mm_load_si128(&vsrc[i]);
    _mm_store_si128(&vdst[i], s);
    m128 su = _mm_load_si128(&vusrc[i]);
    _mm_store_si128(&vudst[i], su);
  }
#endif

  // we dont want to replace our own info from what's in src
  Set(sid_, my_c);
  SetU(sid_, my_u);

  IncU();
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
  // This is the negative case. There is new information to give to the sync.
  Sid tc = child->last_released_thread_;
  DCHECK_NE(tc, kFreeSid);
  if (LIKELY(child->GetU(tc) > GetU(tc))) {

#if TSAN_UCLOCK_MEASUREMENTS
    atomic_fetch_add(&ctx->num_uclock_acquires, 1, memory_order_relaxed);
#endif

    // Join as per normal (because checking C_l ⊑ C_t takes as much time as joining)
    bool did_acquire = false;
#if !TSAN_VECTORIZE
    for (uptr i = 0; i < kThreadSlotCount; i++) {
      uclk_[i] = max(uclk_[i], child->uclk_[i]);
      Epoch cc = child->clk_[i];
      Epoch pc = clk_[i];
      clk_[i] = max(cc, pc);
      did_acquire |= cc > pc;
    }
#else
    m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
    m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(child->clk_);
    m128* __restrict vudst = reinterpret_cast<m128*>(uclk_);
    m128 const* __restrict vusrc = reinterpret_cast<m128 const*>(child->uclk_);
    for (uptr i = 0; i < kVectorClockSize; i++) {
      m128 s = _mm_load_si128(&vsrc[i]);
      m128 d = _mm_load_si128(&vdst[i]);
      m128 m = _mm_max_epu16(s, d);
      _mm_store_si128(&vdst[i], m);

      m128 su = _mm_load_si128(&vusrc[i]);
      m128 du = _mm_load_si128(&vudst[i]);
      m128 mu = _mm_max_epu16(su, du);
      _mm_store_si128(&vudst[i], mu);

      // if all in max are the same as all in dst, then diff will be all zeros, and that means we didnt acquire anything
      // in other words, if not all zeros, means we acquired something
      m128 diff = _mm_xor_si128(d, m);
      did_acquire |= !_mm_test_all_zeros(diff, diff);
    }
#endif

    // Unlike the Release variants above, we don't care about local_for_release, because the thread called ReleaseStore to release
    // to `child`, which already accounts for local_for_release.

    // If learnt something new about the lock, increment augmented epoch to signal that future releases will give new information
    if (LIKELY(did_acquire)) IncU();
  }
  // else {
  //   // Verify that it is correct. If we skip the acquire, we should truly know about everything the lock knows.
  //   for (uptr i = 0; i < kThreadSlotCount; ++i) {
  //     DCHECK_GE(clk_[i], child->clk_[i]);
  //   }
  // }
}
#endif

VectorClock& VectorClock::operator=(const VectorClock& other) {
#if TSAN_EMPTY
  return *this;
#endif
#if TSAN_UCLOCKS
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = other.clk_[i];
    uclk_[i] = other.uclk_[i];
  }
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
  m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(other.clk_);
  m128* __restrict vudst = reinterpret_cast<m128*>(uclk_);
  m128 const* __restrict vusrc = reinterpret_cast<m128 const*>(other.uclk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    m128 s = _mm_load_si128(&vsrc[i]);
    _mm_store_si128(&vdst[i], s);
    m128 su = _mm_load_si128(&vusrc[i]);
    _mm_store_si128(&vudst[i], su);
  }
#endif
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

#if TSAN_OL
SyncClock::SyncClock() {
    clock_ = nullptr;
    u_ = kEpochZero;
    last_release_was_store_ = true;
    last_release_was_atomic_ = false;
}

SyncClock::~SyncClock() {
  if (LIKELY(clock_)) clock_->DropRef();
}


#if TSAN_OL_MEASUREMENTS
void SharedClock::HoldRef() {
  atomic_fetch_add(&ctx->num_holds, 1, memory_order_relaxed);
  atomic_fetch_add(&ref_cnt, 1, memory_order_relaxed);
}

void SharedClock::DropRef(SharedClockAlloc& alloc) {
  DCHECK_GT(atomic_load_relaxed(&ref_cnt), 0);
  atomic_fetch_add(&ctx->num_drops, 1, memory_order_relaxed);
  if (atomic_fetch_sub(&ref_cnt, 1, memory_order_relaxed) == 1) {
    atomic_fetch_add(&ctx->num_frees, 1, memory_order_relaxed);
    alloc.free(this);
    return;
  }
}
#endif

ALWAYS_INLINE SharedClock::SharedClock() {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_deep_copies, 1, memory_order_relaxed);
#endif
  atomic_store_relaxed(&ref_cnt, 1);
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = kEpochZero;
    next_[i] = static_cast<Sid>(i+1);
    prev_[i] = static_cast<Sid>(i-1);
  }
#else
  m128 z = _mm_setzero_si128();
  m128* vclk = reinterpret_cast<m128*>(clk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    _mm_store_si128(&vclk[i], z);
  }
  // TOOD(dwslim): This can be vectorized but this function is probably rarely called anyway.
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    next_[i] = static_cast<Sid>(i+1);
    prev_[i] = static_cast<Sid>(i-1);
  }
#endif

  head_ = static_cast<Sid>(0);
}

ALWAYS_INLINE SharedClock::SharedClock(const SharedClock* clock) {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_deep_copies, 1, memory_order_relaxed);
#endif
  atomic_store_relaxed(&ref_cnt, 1);
  *this = *clock;
}

ALWAYS_INLINE SharedClock::SharedClock(const SharedClock* clock_t, const SharedClock* clock_l) {
#if TSAN_OL_MEASUREMENTS
  atomic_fetch_add(&ctx->num_deep_copies, 1, memory_order_relaxed);
#endif
  // CHECK(clock_l);
  atomic_store_relaxed(&ref_cnt, 1);
  *this = *clock_l;
  Join(clock_t);
}

ALWAYS_INLINE void SharedClock::Join(const SharedClock* other) {
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    Epoch cti = other->clk_[i];
    if (clk_[i] < cti) {
      Set(i, cti);
    }
  }
}

ALWAYS_INLINE SharedClock& SharedClock::operator=(const SharedClock& other) {
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = other.clk_[i];
    next_[i] = other.next_[i];
    prev_[i] = other.prev_[i];
  }
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
  m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(other.clk_);
  for (uptr i = 0; i < kVectorClockSize; i++) {
    m128 s = _mm_load_si128(&vsrc[i]);
    _mm_store_si128(&vdst[i], s);
  }

  m128* __restrict vndst = reinterpret_cast<m128*>(next_);
  m128 const* __restrict vnsrc = reinterpret_cast<m128 const*>(other.next_);
  m128* __restrict vpdst = reinterpret_cast<m128*>(prev_);
  m128 const* __restrict vpsrc = reinterpret_cast<m128 const*>(other.prev_);
  for (uptr i = 0; i < kOrderedListNodeSize; i++) {
    m128 ns = _mm_load_si128(&vnsrc[i]);
    _mm_store_si128(&vndst[i], ns);
    m128 ps = _mm_load_si128(&vpsrc[i]);
    _mm_store_si128(&vpdst[i], ps);
  }
#endif
  head_ = other.head_;

  return *this;
}

void SharedClock::DropRef() {
  DCHECK_GT(atomic_load_relaxed(&ref_cnt), 0);
  if (atomic_load(&ref_cnt, memory_order_acquire) == 1 ||
      atomic_fetch_sub(&ref_cnt, 1, memory_order_acq_rel) == 1) {
      Lock lock(&ctx->shared_clock_free_list_mtx);
      free_next_ = ctx->shared_clock_free_list;
      ctx->shared_clock_free_list = this;
      ctx->num_free_shared_clock++;
  }
}

SharedClockAlloc::SharedClockAlloc() {
  // Refill();
  pool_cur_ = pool_end_ = free_list_ = nullptr;

  {
    Lock l(&ctx->shared_clock_alloc_mtx);
    if (!ctx->shadow_alloc_queue.Empty()) {
      SharedClockAlloc* alloc = ctx->shadow_alloc_queue.PopFront();
      Copy(alloc);
      Free(alloc);
    }
  }
}

SharedClockAlloc::~SharedClockAlloc() {
  {
    Lock l(&ctx->shared_clock_alloc_mtx);
    SharedClockAlloc* alloc = New<SharedClockAlloc>(this);
    ctx->shadow_alloc_queue.PushFront(alloc);
  }
}

ALWAYS_INLINE void SharedClockAlloc::Refill() {
  // Take existing pool from queue
  {
    Lock l(&ctx->shared_clock_alloc_mtx);
    if (!ctx->shadow_alloc_queue.Empty()) {
      SharedClockAlloc* alloc = ctx->shadow_alloc_queue.PopFront();
      Copy(alloc);
      Free(alloc);
      return;
    }
  }

  // Take free list from ctx if it is big
  {
    Lock l(&ctx->shared_clock_free_list_mtx);
    if (ctx->num_free_shared_clock >= 1024) {
      free_list_ = ctx->shared_clock_free_list;
      ctx->shared_clock_free_list = nullptr;
      ctx->num_free_shared_clock = 0;
      return;
    }
  }

  // if we didnt get any existing pools from above, make a new pool
  pool_cur_ = (SharedClock*) MmapOrDie(kSize * sizeof(SharedClock), "SharedClockAlloc");
  pool_end_ = pool_cur_ + kSize;
}

template <typename... Args>
ALWAYS_INLINE SharedClock* SharedClockAlloc::Make(Args &&...args) {
  return new (next()) SharedClock(static_cast<Args &&>(args)...);
}
#endif

}  // namespace __tsan
