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
#ifndef TSAN_SHARED_CLOCK_H
#define TSAN_SHARED_CLOCK_H

#include "tsan_defs.h"

namespace __tsan {

void FreeImpl(void *p);

// Fixed-size vector clock, used both for threads and sync objects.
class SharedClock {
 public:
  SharedClock();
  SharedClock(const SharedClock*);
  SharedClock(const SharedClock*, const SharedClock*);

  Epoch Get(Sid sid) const;
  void Set(Sid sid, Epoch v);
  Epoch Get(u8 sid) const;
  void Set(u8 sid, Epoch v);
  void SetOnly(u8 sid, Epoch v);

  Sid head() const;
  void SetHead(Sid head);
  Sid Next(Sid) const;

  void HoldRef();
  void DropRef();

  void Join(const SharedClock* other);
  bool IsShared() const;

  SharedClock& operator=(const SharedClock& other);

 private:
  Epoch clk_[kThreadSlotCount] VECTOR_ALIGNED;
  Sid next_[kThreadSlotCount] VECTOR_ALIGNED;
  Sid prev_[kThreadSlotCount] VECTOR_ALIGNED;
  Sid head_;
  atomic_uint16_t ref_cnt;
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

ALWAYS_INLINE void SharedClock::DropRef() {
  DCHECK_GT(atomic_load_relaxed(&ref_cnt), 0);
  if (atomic_fetch_sub(&ref_cnt, 1, memory_order_relaxed) == 1) FreeImpl(this);
}
#endif

ALWAYS_INLINE bool SharedClock::IsShared() const {
  DCHECK_GT(atomic_load_relaxed(&ref_cnt), 0);
  return atomic_load_relaxed(&ref_cnt) != 1;
}
}  // namespace __tsan

#endif  // TSAN_SHARED_CLOCK_H
