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

namespace __tsan {

// Fixed-size vector clock, used both for threads and sync objects.
class VectorClock {
 public:
  VectorClock();

  Epoch Get(Sid sid) const;
  void Set(Sid sid, Epoch v);

#if TSAN_SAMPLING
  Epoch GetUclk(Sid sid) const;
  void SetUclk(Sid sid, Epoch v);

  Sid GetSid() const;
  void SetSid(Sid sid);
#endif

  void Reset();
  void Acquire(const VectorClock* src);
  void Release(VectorClock** dstp);
  void ReleaseStore(VectorClock** dstp);
#if TSAN_SAMPLING
  void ReleaseStoreAtomic(VectorClock** dstp);
  // void IncClk();
  Epoch IncUclk();
  void ReleaseFork(VectorClock** dstp);
  void AcquireJoin(const VectorClock* src);
#endif
  void ReleaseStoreAcquire(VectorClock** dstp);
  void ReleaseAcquire(VectorClock** dstp);

  VectorClock& operator=(const VectorClock& other);

 private:
  Epoch clk_[kThreadSlotCount] VECTOR_ALIGNED;
#if TSAN_SAMPLING
  Epoch uclk_[kThreadSlotCount] VECTOR_ALIGNED;
#endif

#if TSAN_SAMPLING
  // only used by threads
  Sid sid_;

  // only used by locks
  Sid last_released_thread_;
#endif
};

ALWAYS_INLINE Epoch VectorClock::Get(Sid sid) const {
  return clk_[static_cast<u8>(sid)];
}

ALWAYS_INLINE void VectorClock::Set(Sid sid, Epoch v) {
  DCHECK_GE(v, clk_[static_cast<u8>(sid)]);
  clk_[static_cast<u8>(sid)] = v;
}

#if TSAN_SAMPLING
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
  CHECK_LT(v, static_cast<u16>(kEpochOver) << 1);
  uclk_[static_cast<u8>(sid)] = v;
}

ALWAYS_INLINE Sid VectorClock::GetSid() const {
  return sid_;
}

ALWAYS_INLINE void VectorClock::SetSid(Sid sid) {
  sid_ = sid;
}

// ALWAYS_INLINE void VectorClock::IncClk() {
//   Epoch epoch = EpochInc(Get(sid_));
//   CHECK(!EpochOverflow(epoch));
//   Set(sid_, epoch);
// }

ALWAYS_INLINE Epoch VectorClock::IncUclk() {
  Epoch epoch = EpochInc(GetUclk(sid_));
  CHECK(!EpochOverflow(epoch));
  SetUclk(sid_, epoch);
  return epoch;
}
#endif

}  // namespace __tsan

#endif  // TSAN_VECTOR_CLOCK_H
