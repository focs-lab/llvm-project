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
#ifndef TSAN_SYNC_CLOCK_H
#define TSAN_SYNC_CLOCK_H

#include "tsan_defs.h"
#include "tsan_shared_clock.h"

namespace __tsan {

// Fixed-size vector clock, used both for threads and sync objects.
class SyncClock {
 public:
  SyncClock();

  Epoch u() const;
  void SetU(Epoch u);

  bool LastReleaseWasStore() const;
  void SetLastReleaseWasStore();
  void ClearLastReleaseWasStore();

  bool LastReleaseWasAtomic() const;
  void SetLastReleaseWasAtomic();
  void ClearLastReleaseWasAtomic();

  Sid LastReleasedThread() const;
  void SetLastReleasedThread(Sid sid);

  SharedClock* clock() const;
  void SetClock(SharedClock* clock);

  SyncClock& operator=(const SyncClock& other);

 private:
    SharedClock* clock_;
    Epoch u_;
    Sid last_released_thread_;
    bool last_release_was_store_;
    bool last_release_was_atomic_;
};

ALWAYS_INLINE SyncClock::SyncClock() {
    clock_ = nullptr;
    u_ = kEpochZero;
    last_release_was_store_ = true;
    last_release_was_atomic_ = false;
}

ALWAYS_INLINE Epoch SyncClock::u() const {
    return u_;
}

ALWAYS_INLINE void SyncClock::SetU(Epoch u) {
    u_ = u;
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

ALWAYS_INLINE void SyncClock::SetClock(SharedClock* clock) {
    if (clock_) clock_->DropRef();
    clock_ = clock;
    clock->HoldRef();
}
}  // namespace __tsan

#endif  // TSAN_SHARED_CLOCK_H
