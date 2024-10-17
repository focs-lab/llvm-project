//===-- tsan_vector_clock.h -------------------------------------*- C++ -*-===//
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
#ifndef TSAN_VECTOR_CLOCK_H
#define TSAN_VECTOR_CLOCK_H

#include "tsan_defs.h"
#if TSAN_OL
#include "tsan_ilist.h"
#endif

namespace __tsan {

#if TSAN_OL
// Fixed-size vector clock, used both for threads and sync objects.

class SharedClockAlloc;

class SharedClock {
 public:
  SharedClock();
  SharedClock(const SharedClock*);
  SharedClock(const SharedClock*, const SharedClock*);

  Epoch Get(Sid sid) const;
  void Set(Sid sid, Epoch v);
  Epoch Get(u8 sid) const;
  void Set(u8 sid, Epoch v);
  void SetOnly(Sid sid, Epoch v);
  void SetOnly(u8 sid, Epoch v);

  Sid head() const;
  void SetHead(Sid head);
  Sid Next(Sid) const;

  void HoldRef();
  void DropRef();
  void DropRef(SharedClockAlloc& alloc);

  void Join(const SharedClock* other);
  bool IsShared() const;

  SharedClock& operator=(const SharedClock& other);

  friend class SharedClockAlloc;

 private:
  Epoch clk_[kThreadSlotCount] VECTOR_ALIGNED;  // 512
  Sid next_[kThreadSlotCount] VECTOR_ALIGNED;   // 256
  Sid prev_[kThreadSlotCount] VECTOR_ALIGNED;   // 256
  Sid head_;
  u8 padding[63];   // so that refcnt is separated from the data
  union {
    atomic_uint64_t ref_cnt;
    SharedClock* free_next_;
  };
  u8 padding2[56];   // so that whole class is cache line aligned
};

class SharedClockAlloc {
 public:
  static constexpr uptr kSize = 1<<18;

  SharedClockAlloc();

  SharedClockAlloc(const SharedClockAlloc* alloc) {
    Copy(alloc);
  }

  ~SharedClockAlloc();

  void Copy(const SharedClockAlloc* alloc) {
    pool_cur_ = alloc->pool_cur_;
    pool_end_ = alloc->pool_end_;
    free_list_ = alloc->free_list_;
  }

  ALWAYS_INLINE SharedClock* next() {
    if (LIKELY(pool_cur_ != pool_end_)) return pool_cur_++;

    if (LIKELY(free_list_ != nullptr)) {
      SharedClock* cur = free_list_;
      free_list_ = free_list_->free_next_;
      return cur;
    }

    // no more
    Refill(); // pool might still be exhausted but free list will have stuff
    return next();
  }

  ALWAYS_INLINE void free(SharedClock* shared_clock) {
    shared_clock->free_next_ = free_list_;
    free_list_ = shared_clock;
  }

  template <typename... Args>
  ALWAYS_INLINE SharedClock *Make(Args &&...args);

  INode node;

 private:
  static constexpr uptr kPoolSize = kSize * sizeof(SharedClock);
  static_assert(sizeof(SharedClock) % SANITIZER_CACHE_LINE_SIZE == 0);
  void Refill();

  SharedClock* pool_cur_;
  SharedClock* pool_end_;
  SharedClock* free_list_;
  // int refills = 0;
};

ALWAYS_INLINE void SharedClock::SetHead(Sid head) {
  DCHECK_NE(head, kFreeSid);
  if (head_ == head) return;
  prev_[static_cast<u8>(head_)] = head;
  next_[static_cast<u8>(head)] = head_;
  prev_[static_cast<u8>(head)] = kFreeSid;
  head_ = head;
}

ALWAYS_INLINE Sid SharedClock::head() const {
  return head_;
}

ALWAYS_INLINE Sid SharedClock::Next(Sid sid) const {
  Sid next = next_[static_cast<u8>(sid)];
  DCHECK_NE(next, sid);
  return next;
}

ALWAYS_INLINE Epoch SharedClock::Get(Sid sid) const {
  return Get(static_cast<u8>(sid));
}

ALWAYS_INLINE void SharedClock::Set(Sid sid, Epoch v) {
  Set(static_cast<u8>(sid), v);
}

ALWAYS_INLINE void SharedClock::Set(u8 sid, Epoch v) {
  DCHECK_EQ(atomic_load_relaxed(&ref_cnt), 1);
  DCHECK_GE(v, clk_[static_cast<u8>(sid)]);
  clk_[sid] = v;

  // push this sid to the head
  // detach
  if (head_ == static_cast<Sid>(sid)) return;
  u8 next_sid = static_cast<u8>(next_[sid]);
  u8 prev_sid = static_cast<u8>(prev_[sid]);
  DCHECK_NE(next_sid, sid);
  DCHECK_NE(prev_sid, sid);
  DCHECK_NE(next_sid, prev_sid);
  if (next_[sid] != kFreeSid) prev_[next_sid] = static_cast<Sid>(prev_sid);
  if (prev_[sid] != kFreeSid) next_[prev_sid] = static_cast<Sid>(next_sid);

  // attach
  SetHead(static_cast<Sid>(sid));

  // u32 sum = 0;
  // Sid curr = head_;
  // for (uptr i = 0; i < kThreadSlotCount; ++i) {
  //   Printf("%u ", curr);
  //   sum += static_cast<u8>(curr);
  //   curr = Next(curr);
  // }
  // Printf("\n");
  // CHECK_EQ(sum, 32640);
}

ALWAYS_INLINE Epoch SharedClock::Get(u8 sid) const {
  return clk_[sid];
}

ALWAYS_INLINE void SharedClock::SetOnly(Sid sid, Epoch v) {
  SetOnly(static_cast<u8>(sid), v);
}

ALWAYS_INLINE void SharedClock::SetOnly(u8 sid, Epoch v) {
  DCHECK_GE(v, clk_[static_cast<u8>(sid)]);
  clk_[sid] = v;
}

// ALWAYS_INLINE Epoch SharedClock::Inc(Sid sid) {
//   return Inc(static_cast<u8>(sid));
// }

// ALWAYS_INLINE Epoch SharedClock::Inc(u8 sid) {
//   DCHECK_NE(sid_, kFreeSid);
//   Epoch epoch = EpochInc(Get(sid));
//   // CHECK(!EpochOverflow(epoch));

//   // Set will push sid to head
//   Set(sid, epoch);

//   return epoch;
// }

#if !TSAN_OL_MEASUREMENTS
ALWAYS_INLINE void SharedClock::HoldRef() {
  atomic_fetch_add(&ref_cnt, 1, memory_order_relaxed);
}

ALWAYS_INLINE void SharedClock::DropRef(SharedClockAlloc& alloc) {
  DCHECK_GT(atomic_load_relaxed(&ref_cnt), 0);
  if (atomic_load(&ref_cnt, memory_order_acquire) == 1 ||
      atomic_fetch_sub(&ref_cnt, 1, memory_order_acq_rel) == 1) alloc.free(this);
}
#endif

ALWAYS_INLINE bool SharedClock::IsShared() const {
  DCHECK_GT(atomic_load_relaxed(&ref_cnt), 0);
  return atomic_load_relaxed(&ref_cnt) != 1;
}

ALWAYS_INLINE SharedClock& SharedClock::operator=(const SharedClock& other) {
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    clk_[i] = other.clk_[i];
    next_[i] = other.next_[i];
    prev_[i] = other.prev_[i];
  }
  head_ = other.head_;

  return *this;
}

class SyncClock {
 public:
  SyncClock();
  ~SyncClock();

  Epoch u() const;
  void SetU(Epoch u);

  Epoch local() const;
  void SetLocal(Epoch epoch);

  Epoch acquired() const;
  void SetAcquired(Epoch epoch);

  Sid acquired_sid() const;
  void SetAcquiredSid(Sid sid);

  bool LastReleaseWasStore() const;
  void SetLastReleaseWasStore();
  void ClearLastReleaseWasStore();

  bool LastReleaseWasAtomic() const;
  void SetLastReleaseWasAtomic();
  void ClearLastReleaseWasAtomic();

  Sid LastReleasedThread() const;
  void SetLastReleasedThread(Sid sid);

  SharedClock* clock() const;
  void SetClock(SharedClock* clock, SharedClockAlloc& alloc);
  void CopyClock(SharedClock* clock, Sid sid, Epoch u);

  SyncClock& operator=(const SyncClock& other);

 private:
    SharedClock* clock_;
    Epoch local_;
    Epoch acquired_;
    Sid acquired_sid_;
    Epoch u_;
    Sid last_released_thread_;
    bool last_release_was_store_;
    bool last_release_was_atomic_;
};

ALWAYS_INLINE Epoch SyncClock::u() const {
    return u_;
}

ALWAYS_INLINE void SyncClock::SetU(Epoch u) {
    u_ = u;
}

ALWAYS_INLINE Epoch SyncClock::local() const {
  return local_;
}

ALWAYS_INLINE void SyncClock::SetLocal(Epoch epoch) {
  local_ = epoch;
}

ALWAYS_INLINE Epoch SyncClock::acquired() const {
  return acquired_;
}

ALWAYS_INLINE void SyncClock::SetAcquired(Epoch epoch) {
  acquired_ = epoch;
}

ALWAYS_INLINE Sid SyncClock::acquired_sid() const {
  return acquired_sid_;
}

ALWAYS_INLINE void SyncClock::SetAcquiredSid(Sid sid) {
  acquired_sid_ = sid;
}

ALWAYS_INLINE bool SyncClock::LastReleaseWasStore() const {
  return last_release_was_store_;
}

ALWAYS_INLINE void SyncClock::SetLastReleaseWasStore() {
  last_release_was_store_ = true;
}

ALWAYS_INLINE void SyncClock::ClearLastReleaseWasStore() {
  last_release_was_store_ = false;
}

ALWAYS_INLINE bool SyncClock::LastReleaseWasAtomic() const {
  return last_release_was_atomic_;
}

ALWAYS_INLINE void SyncClock::SetLastReleaseWasAtomic() {
  last_release_was_atomic_ = true;
}

ALWAYS_INLINE void SyncClock::ClearLastReleaseWasAtomic() {
  last_release_was_atomic_ = false;
}

ALWAYS_INLINE Sid SyncClock::LastReleasedThread() const {
  return last_released_thread_;
}

ALWAYS_INLINE void SyncClock::SetLastReleasedThread(Sid sid) {
  last_released_thread_ = sid;
}

ALWAYS_INLINE SharedClock* SyncClock::clock() const {
  return clock_;
}

ALWAYS_INLINE void SyncClock::SetClock(SharedClock* clock, SharedClockAlloc& alloc) {
  if (clock_ == clock) return;
  if (clock_) clock_->DropRef(alloc);
  clock_ = clock;
  clock->HoldRef();
}
#endif

// Fixed-size vector clock, used both for threads and sync objects.
class VectorClock {
 public:
  VectorClock();
#if TSAN_OL
  ~VectorClock();
#endif

  Epoch Get(Sid sid) const;
  void Set(Sid sid, Epoch v);
  void SetLocal(Sid sid, Epoch v, bool sampled);

#if TSAN_UCLOCKS || TSAN_OL
  Epoch GetU(Sid sid) const;
  void SetU(Sid sid, Epoch v);
  Epoch IncU();

  Sid GetSid() const;
  void SetSid(Sid sid);
#endif

#if TSAN_UCLOCK_MEASUREMENTS
  void BBREAK();
#endif

  void Reset();
#if TSAN_OL
  void BBREAK() const;
  void Acquire(const SyncClock* src);
  void Release(SyncClock** dstp);
  void ReleaseStore(SyncClock** dstp);
  void ReleaseStoreAtomic(SyncClock** dstp);
  void ReleaseFork(SyncClock** dstp);
  void AcquireFromFork(const SyncClock* src);
  void AcquireJoin(const SyncClock* src);
  void ReleaseStoreAcquire(SyncClock** dstp);
  void ReleaseAcquire(SyncClock** dstp);

  Epoch local() const;
  Epoch local_for_release() const;
  void SetLocal(Epoch epoch);

  Epoch acquired() const;
  void SetAcquired(Epoch epoch);

  Sid acquired_sid() const;
  void SetAcquiredSid(Sid sid);

  void Unshare();
  bool IsShared() const;

  SharedClockAlloc& Alloc();
#elif TSAN_UCLOCKS
  void Acquire(VectorClock* src);
  void Release(VectorClock** dstp);
  void ReleaseStore(VectorClock** dstp);
  void ReleaseStoreAcquire(VectorClock** dstp);
  void ReleaseAcquire(VectorClock** dstp);
  void ReleaseStoreAtomic(VectorClock** dstp);
  // void IncClk();
  void ReleaseFork(VectorClock** dstp);
  void AcquireFromFork(const VectorClock* src);
  void AcquireJoin(const VectorClock* src);

  Epoch local_for_release() const;
#else
  void Acquire(const VectorClock* src);
  void Release(VectorClock** dstp);
  void ReleaseStore(VectorClock** dstp);
  void ReleaseStoreAcquire(VectorClock** dstp);
  void ReleaseAcquire(VectorClock** dstp);
#endif

#if TSAN_UCLOCKS || TSAN_OL
  bool sampled() const;
  void SetSampled(bool sampled);
#endif

  VectorClock& operator=(const VectorClock& other);

 private:
#if TSAN_OL
  Epoch uclk_[kThreadSlotCount] VECTOR_ALIGNED;
  SharedClock* clock_;
  Epoch local_;
  Epoch acquired_;
  Sid acquired_sid_;
  bool is_shared_;

  SharedClockAlloc alloc_;

  void AcquireToDirty(Sid sid, Epoch epoch);
#elif TSAN_UCLOCKS
  Epoch clk_[kThreadSlotCount] VECTOR_ALIGNED;
  Epoch uclk_[kThreadSlotCount] VECTOR_ALIGNED;

  // only used by syncs
  Sid last_released_thread_;
  Sid last_acquired_thread_;
  bool last_release_was_store_;
#else
  Epoch clk_[kThreadSlotCount] VECTOR_ALIGNED;
#endif

#if TSAN_UCLOCKS || TSAN_OL
  // only used by threads
  Sid sid_;
  bool sampled_;
#endif
};

#if TSAN_OL
ALWAYS_INLINE Epoch VectorClock::Get(Sid sid) const {
  return clock_->Get(sid);
}

ALWAYS_INLINE void VectorClock::Set(Sid sid, Epoch v) {
  DCHECK(!IsShared());
  DCHECK(!clock_->IsShared());
  clock_->Set(sid, v);
  DCHECK_EQ(clock_->head(), sid);
}
#else
ALWAYS_INLINE Epoch VectorClock::Get(Sid sid) const {
  return clk_[static_cast<u8>(sid)];
}

ALWAYS_INLINE void VectorClock::Set(Sid sid, Epoch v) {
  DCHECK_GE(v, clk_[static_cast<u8>(sid)]);
  clk_[static_cast<u8>(sid)] = v;
}

// This is no different from Set.
// However the main purpose of this is to make clear the semantics through
// the function name and the DCHECK.
// This will only be called after an array join/copy which ignores whether
// the thread has sampled. We need to replace that entry with the result
// of local_for_release, which may not be monotonic.
// sampled is only used in debug mode
ALWAYS_INLINE void VectorClock::SetLocal(Sid sid, Epoch v, bool sampled) {
  DCHECK_EQ(v, static_cast<u16>(clk_[static_cast<u8>(sid)]) - !sampled);
  clk_[static_cast<u8>(sid)] = v;
}
#endif

#if TSAN_OL
ALWAYS_INLINE Epoch VectorClock::GetU(Sid sid) const {
  return uclk_[static_cast<u8>(sid)];
}

ALWAYS_INLINE void VectorClock::SetU(Sid sid, Epoch v) {
  DCHECK_GE(v, uclk_[static_cast<u8>(sid)]);
  uclk_[static_cast<u8>(sid)] = v;
}

ALWAYS_INLINE Sid VectorClock::GetSid() const {
  return sid_;
}

ALWAYS_INLINE void VectorClock::SetSid(Sid sid) {
  sid_ = sid;
}

ALWAYS_INLINE Epoch VectorClock::IncU() {
  DCHECK_NE(sid_, kFreeSid);
  Epoch epoch = GetU(sid_);
  DCHECK_GT(EpochInc(epoch), epoch);
  epoch = EpochInc(GetU(sid_));
  SetU(sid_, epoch);
  return epoch;
}

ALWAYS_INLINE bool VectorClock::IsShared() const {
  // if (is_shared_ < clock_->IsShared()) BBREAK();
  // A lock may drop ref concurrently
  DCHECK_GE(is_shared_, clock_->IsShared());
  return is_shared_;
}

ALWAYS_INLINE Epoch VectorClock::local() const {
  return local_;
}

ALWAYS_INLINE Epoch VectorClock::local_for_release() const {
  CHECK_GT(local_, kEpochZero);
  // if (UNLIKELY(sampled_)) return local;
  // not sure if this optimization is actually useful, but i want to avoid if statements
  // if sampled then send e, else send e-1
  return static_cast<Epoch>(static_cast<u16>(local_) - !sampled_);
}

ALWAYS_INLINE void VectorClock::SetLocal(Epoch epoch) {
  local_ = epoch;
}

ALWAYS_INLINE Epoch VectorClock::acquired() const {
  return acquired_;
}

ALWAYS_INLINE void VectorClock::SetAcquired(Epoch epoch) {
  acquired_ = epoch;
}

ALWAYS_INLINE Sid VectorClock::acquired_sid() const {
  return acquired_sid_;
}

ALWAYS_INLINE void VectorClock::SetAcquiredSid(Sid sid) {
  acquired_sid_ = sid;
}

ALWAYS_INLINE SharedClockAlloc& VectorClock::Alloc() {
  return alloc_;
}

#elif TSAN_UCLOCKS
ALWAYS_INLINE Epoch VectorClock::local_for_release() const {
  Epoch local = Get(sid_);
  DCHECK_GT(local, kEpochZero);
  // if (UNLIKELY(sampled_)) return local;
  // not sure if this optimization is actually useful, but i want to avoid if statements
  // if sampled then send e, else send e-1
  return static_cast<Epoch>(static_cast<u16>(local) - !sampled_);
}

ALWAYS_INLINE Epoch VectorClock::GetU(Sid sid) const {
  return uclk_[static_cast<u8>(sid)];
}

ALWAYS_INLINE void VectorClock::SetU(Sid sid, Epoch v) {
  DCHECK_GE(v, uclk_[static_cast<u8>(sid)]);
  // Epoch has 16 bits. It is ok to be above kEpochLast.
  // fast_state.uclk_overflowed_ will be true once uclk is above kEpochLast.
  // This should give plenty of room for slot to detach.
  // If slot is not detached even after so many "grace-period" increments, there
  // is clearly something wrong.
  DCHECK_LT(v, static_cast<u16>(kUEpochMax) + kThreadSlotCount);
  uclk_[static_cast<u8>(sid)] = v;
}

ALWAYS_INLINE Sid VectorClock::GetSid() const {
  return sid_;
}

ALWAYS_INLINE void VectorClock::SetSid(Sid sid) {
  sid_ = sid;
}

ALWAYS_INLINE Epoch VectorClock::IncU() {
  DCHECK_NE(sid_, kFreeSid);
  Epoch epoch = EpochInc(GetU(sid_));
  SetU(sid_, epoch);
  return epoch;
}
#endif

#if TSAN_UCLOCKS || TSAN_OL
ALWAYS_INLINE bool VectorClock::sampled() const {
  return sampled_;
}

ALWAYS_INLINE void VectorClock::SetSampled(bool sampled) {
  sampled_ = sampled;
}
#endif

}  // namespace __tsan

#endif  // TSAN_VECTOR_CLOCK_H
