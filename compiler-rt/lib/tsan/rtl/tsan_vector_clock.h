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
#include "tsan_shared_clock.h"
#include "tsan_sync_clock.h"

namespace __tsan {

// Fixed-size vector clock, used both for threads and sync objects.
class VectorClock {
 public:
  VectorClock();

  Epoch Get(Sid sid) const;
  void Set(Sid sid, Epoch v);

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

  void Unshare();
  bool IsShared() const;
#else
  void Acquire(const VectorClock* src);
  void Release(VectorClock** dstp);
  void ReleaseStore(VectorClock** dstp);
#if TSAN_UCLOCKS
  void ReleaseStoreAtomic(VectorClock** dstp);
  // void IncClk();
  void ReleaseFork(VectorClock** dstp);
  void AcquireFromFork(const VectorClock* src);
  void AcquireJoin(const VectorClock* src);
#endif
  void ReleaseStoreAcquire(VectorClock** dstp);
  void ReleaseAcquire(VectorClock** dstp);
#endif

  VectorClock& operator=(const VectorClock& other);

 private:
#if TSAN_OL
  Epoch uclk_[kThreadSlotCount] VECTOR_ALIGNED;
  SharedClock* clock_;
  bool is_shared_;
#else
  Epoch clk_[kThreadSlotCount] VECTOR_ALIGNED;
#if TSAN_UCLOCKS
  Epoch uclk_[kThreadSlotCount] VECTOR_ALIGNED;
#endif
#endif

#if TSAN_UCLOCKS || TSAN_OL
  // only used by threads
  Sid sid_;
#endif

#if TSAN_UCLOCKS
  // only used by syncs
  Sid last_released_thread_;
  bool last_release_was_store_;
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
  Epoch epoch = EpochInc(GetU(sid_));
  SetU(sid_, epoch);
  return epoch;
}

ALWAYS_INLINE bool VectorClock::IsShared() const {
  // if (is_shared_ < clock_->IsShared()) BBREAK();
  // A lock may drop ref concurrently
  DCHECK_GE(is_shared_, clock_->IsShared());
  return is_shared_;
}
#elif TSAN_UCLOCKS
ALWAYS_INLINE Epoch VectorClock::GetUclk(Sid sid) const {
  return uclk_[static_cast<u8>(sid)];
}

ALWAYS_INLINE void VectorClock::SetUclk(Sid sid, Epoch v) {
  DCHECK_GE(v, uclk_[static_cast<u8>(sid)]);
  // Epoch has 16 bits. It is ok to be above kEpochLast.
  // fast_state.uclk_overflowed_ will be true once uclk is above kEpochLast.
  // This should give plenty of room for slot to detach.
  // If slot is not detached even after so many "grace-period" increments, there
  // is clearly something wrong.
  DCHECK_LT(v, static_cast<u16>(kEpochOver) << 1);
  uclk_[static_cast<u8>(sid)] = v;
}

ALWAYS_INLINE Sid VectorClock::GetSid() const {
  return sid_;
}

ALWAYS_INLINE void VectorClock::SetSid(Sid sid) {
  sid_ = sid;
}

ALWAYS_INLINE void VectorClock::IncClk() {
  Epoch epoch = EpochInc(Get(sid_));
  DCHECK(!EpochOverflow(epoch));
  Set(sid_, epoch);
}
#endif

}  // namespace __tsan

#endif  // TSAN_VECTOR_CLOCK_H
