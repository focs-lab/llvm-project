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

#if TSAN_MINJIAN
  Epoch GetUclk(Sid sid) const;
  void SetUclk(Sid sid, Epoch v);

  Sid GetSid() const;
  void SetSid(Sid sid);

  bool HasSampled() const;
  void SetSampled();
#endif

  void Reset();
  void Acquire(const VectorClock* src);
  void Release(VectorClock** dstp);
  void ReleaseStore(VectorClock** dstp);
#if TSAN_MINJIAN
  void ReleaseStoreAtomic(VectorClock** dstp);
#endif
  void ReleaseStoreAcquire(VectorClock** dstp);
  void ReleaseAcquire(VectorClock** dstp);

  VectorClock& operator=(const VectorClock& other);

 private:
  Epoch clk_[kThreadSlotCount] VECTOR_ALIGNED;
#if TSAN_MINJIAN
  Epoch uclk_[kThreadSlotCount] VECTOR_ALIGNED;
#endif

#if TSAN_MINJIAN
  // only used by threads
  Sid sid_;
  bool has_sampled_;

  // only used by locks
  Epoch lock_uclk_;
  Sid last_released_;
#endif
};

ALWAYS_INLINE Epoch VectorClock::Get(Sid sid) const {
  return clk_[static_cast<u8>(sid)];
}

ALWAYS_INLINE void VectorClock::Set(Sid sid, Epoch v) {
  DCHECK_GE(v, clk_[static_cast<u8>(sid)]);
  clk_[static_cast<u8>(sid)] = v;
}

#if TSAN_MINJIAN
ALWAYS_INLINE Epoch VectorClock::GetUclk(Sid sid) const {
  return uclk_[static_cast<u8>(sid)];
}

ALWAYS_INLINE void VectorClock::SetUclk(Sid sid, Epoch v) {
  DCHECK_GE(v, uclk_[static_cast<u8>(sid)]);
  uclk_[static_cast<u8>(sid)] = v;
}

ALWAYS_INLINE Sid VectorClock::GetSid() const {
  return sid_;
}

ALWAYS_INLINE void VectorClock::SetSid(Sid sid) {
  sid_ = sid;
}

ALWAYS_INLINE bool VectorClock::HasSampled() const {
  return has_sampled_;
}

ALWAYS_INLINE void VectorClock::SetSampled() {
  has_sampled_ = true;
}
#endif

}  // namespace __tsan

#endif  // TSAN_VECTOR_CLOCK_H
