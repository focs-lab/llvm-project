//===-- psan_vector_clock.h -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PredictiveSanitizer (PSan), a predictive race detector.
// This is a fork of ThreadSanitizer (TSan) at LLVM commit
// c609043dd00955bf177ff57b0bad2a87c1e61a36.
//
//===----------------------------------------------------------------------===//
#ifndef PSAN_VECTOR_CLOCK_H
#define PSAN_VECTOR_CLOCK_H

#include "psan_defs.h"

namespace __psan {

// Fixed-size vector clock, used both for threads and sync objects.
class VectorClock {
 public:
  VectorClock();

  Epoch Get(Sid sid) const;
  void Set(Sid sid, Epoch v);

  void Reset();
  void Acquire(const VectorClock* src);
  void Release(VectorClock** dstp) const;
  void ReleaseStore(VectorClock** dstp) const;
  void ReleaseStoreAcquire(VectorClock** dstp);
  void ReleaseAcquire(VectorClock** dstp);

  VectorClock& operator=(const VectorClock& other);

 private:
  Epoch clk_[kThreadSlotCount] VECTOR_ALIGNED;
};

ALWAYS_INLINE Epoch VectorClock::Get(Sid sid) const {
  return clk_[static_cast<u8>(sid)];
}

ALWAYS_INLINE void VectorClock::Set(Sid sid, Epoch v) {
  DCHECK_GE(v, clk_[static_cast<u8>(sid)]);
  clk_[static_cast<u8>(sid)] = v;
}

}  // namespace __psan

#endif  // PSAN_VECTOR_CLOCK_H
