//===-- psan_hb.cpp -------------------------------------------------------===//
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
#include "psan_hb.h"
#include "../psan_rtl.h"
#include "../psan_mman.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_placement_new.h"

namespace __psan {
// TODO(dwslim): Can't put this in psan_hb.h because including sanitizer_placement_new.h
// will cause the unit tests to fail building.
// Anyway, need to measure to know if inability to inline this function affects performance.
Shadow Shadow::MakeHBShadowCell() {
  return Shadow(New<HBShadowCell>());
}


HBEpoch HBShadowCell::HandleRead(ThreadState *thr, HBEpoch cur) {
  // ReadLock rl(&mtx_);

  uptr addr, size;
  cur.GetAccess(&addr, &size, nullptr);

  // Printf("read thr.sid: %u, &this: %p, addr: %u, size: %u\n", thr->fast_state.sid(), this, addr, size);
  // for (u8 i = 0; i < size; ++i) {
    // HBShadow* hb_shadow = shadow(addr+i);
    HBShadow* hb_shadow = shadow(addr);
    HBEpoch race = hb_shadow->HandleRead(thr, cur);
    if (race.raw() != HBEpoch::kEmpty) {
      // Printf("Race!\n");
      return race;
    }
  // }

  return HBEpoch(HBEpoch::kEmpty);
}

HBEpoch HBShadowCell::HandleWrite(ThreadState *thr, HBEpoch cur) {
  // ReadLock rl(&mtx_);

  uptr addr, size;
  cur.GetAccess(&addr, &size, nullptr);

  // Printf("write thr.sid: %u, &this: %p, addr: %u, size: %u\n", thr->fast_state.sid(), this, addr, size);
  // for (u8 i = 0; i < size; ++i) {
    // HBShadow* hb_shadow = shadow(addr+i);
    HBShadow* hb_shadow = shadow(addr);
    HBEpoch race = hb_shadow->HandleWrite(thr, cur);
    if (race.raw() != HBEpoch::kEmpty) {
      // Printf("Race! %p, %p\n", race.raw(), cur.raw());
      return race;
    }
  // }

  return HBEpoch(HBEpoch::kEmpty);
}

ALWAYS_INLINE USED bool CheckRace(ThreadState *thr, HBEpoch cur, HBEpoch old) {
  Sid sid = cur.sid();
  uptr addr, size;
  AccessType typ;
  cur.GetAccess(&addr, &size, &typ);

  // no writes before
  if (LIKELY(old.raw() == HBEpoch::kEmpty)) return false;

  // same thread accessing
  if (LIKELY(old.sid() == sid)) return false;

  if (LIKELY(old.IsBothAtomic(typ))) return false;

  // if HB-ordered then not a race
  if (LIKELY(thr->clock.Get(old.sid()) >= old.epoch())) return false;

  return true;
}

ALWAYS_INLINE HBEpoch HBShadow::HandleRead(ThreadState *thr, HBEpoch cur) {
  Sid sid = cur.sid();
  uptr addr, size;
  AccessType typ;
  cur.GetAccess(&addr, &size, &typ);

  SpinMutexLock lock(&mtx_);

  // Consider the scenario where the wx is written just right when we reach this check.
  // Neither will we detect that write, nor will that write detect us.
  // {
    // Lock lock(&wmtx_);
    // HBEpoch old_wx = HBEpoch(LoadHBEpoch(wx_p()));
    HBEpoch old_wx = wx_;
    if (CheckRace(thr, cur, old_wx)) return old_wx;
  // }

  // Finished checking race. Now update read epoch if necessary.

  // If we want to check only and don't store
  if ((typ & kAccessCheckOnly)) return HBEpoch(HBEpoch::kEmpty);

  // Update the read epoch, we may need to transition to vector clock
  // RawHBEpoch* rxp = rx_p();
  // StoreHBEpoch(rxp, cur.raw());
  // {
    // Lock because consider 2 threads doing the following concurrently:
    // 1. Update rx because same sid
    // 2. Transition to VC because different sid
    // Lock lock(&rmtx_);
    // RawHBEpoch* rxp = rx_p();
    // HBEpoch old_r = HBEpoch(LoadHBEpoch(rxp));
    HBEpoch old_r = rx_;
    if (old_r.raw() == HBEpoch::kEmpty || sid == old_r.sid()) rx_ = cur;
    else {
      // RawHBEpoch* rv = rv_p();
      // We store the epochs into the VC first, then update rx with ReadSharedMarker with release order
      // So, when a write event loads rx with acquire order, it will also see the VC with its latest updates,
      // without needing to lock the mutex.
      // StoreHBEpoch(&rv[static_cast<u8>(cur.sid())], cur.raw());
      // if (old_r.raw() != HBEpoch::ReadSharedMarker()) {
      //   StoreHBEpoch(&rv[static_cast<u8>(old_r.sid())], old_r.raw());
      //   StoreReleaseHBEpoch(rxp, HBEpoch::ReadSharedMarker());
      // }

      // StoreHBEpoch(&rv[rv_curr], cur.raw());
      rv_[rv_curr] = cur;
      rv_curr = (rv_curr + 1) & (kRvSize - 1);
      // we have not transitioned before
      if (old_r.raw() != HBEpoch::ReadSharedMarker()) {
        // StoreHBEpoch(&rv[rv_curr], old_r.raw());
        rv_[rv_curr] = old_r;
        rv_curr = (rv_curr + 1) & (kRvSize - 1);
        // StoreReleaseHBEpoch(rxp, HBEpoch::ReadSharedMarker());
        rx_ = HBEpoch(HBEpoch::ReadSharedMarker());
      }
    }
  // }

  return HBEpoch(HBEpoch::kEmpty);
}

ALWAYS_INLINE HBEpoch HBShadow::HandleWrite(ThreadState *thr, HBEpoch cur) {
  // Printf("Handling write\n");
  uptr addr, size;
  AccessType typ;
  cur.GetAccess(&addr, &size, &typ);

  SpinMutexLock lock(&mtx_);

  // Consider the scenario where the wx is written just right when we reach this check.
  // Neither will we detect that write, nor will that write detect us.
  // {
    // Lock lock(&wmtx_);
    // HBEpoch old_wx = HBEpoch(LoadHBEpoch(wx_p()));
    HBEpoch old_wx = wx_;
    if (CheckRace(thr, cur, old_wx)) return old_wx;
  // }

  // Then check race with reads
  {
    // Lock lock(&wmtx_);
    // Load with acquire to see the latest VC if transitioned
    // HBEpoch old_rx = HBEpoch(LoadAcquireHBEpoch(rx_p()));
    HBEpoch old_rx = rx_;
    if (LIKELY(old_rx.raw() != HBEpoch::ReadSharedMarker())) {
      if (CheckRace(thr, cur, old_rx)) return old_rx;
    }
    else {
      for (u32 i = 0; i < kRvSize; ++i) {
        // HBEpoch old_rx = HBEpoch(LoadHBEpoch(&rv_p()[i]));
        HBEpoch old_rx = rv_[i];
        if (old_rx.raw() == HBEpoch::kEmpty) break;
        if (CheckRace(thr, cur, old_rx)) return old_rx;
      }
    }
  }

  // Finished checking race. Now update read epoch if necessary.

  // If we want to check only and don't store
  if ((typ & kAccessCheckOnly)) return HBEpoch(HBEpoch::kEmpty);

  // RawHBEpoch* wxp = wx_p();
  // StoreHBEpoch(wxp, cur.raw());
  wx_ = cur;

  return HBEpoch(HBEpoch::kEmpty);
}
}  // namespace __psan