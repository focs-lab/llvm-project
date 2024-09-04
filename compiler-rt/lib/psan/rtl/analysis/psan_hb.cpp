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
  uptr addr, size;
  cur.GetAccess(&addr, &size, nullptr);

  // Printf("read thr.sid: %u, &this: %p, addr: %u, size: %u\n", thr->fast_state.sid(), this, addr, size);
  for (u8 i = 0; i < size; ++i) {
    HBShadow* hb_shadow = shadow(addr+i);
    HBEpoch race = hb_shadow->HandleRead(thr, cur);
    if (race.raw() != HBEpoch::kEmpty) {
      // Printf("Race!\n");
      return race;
    }
  }

  return HBEpoch(HBEpoch::kEmpty);
}

HBEpoch HBShadowCell::HandleWrite(ThreadState *thr, HBEpoch cur) {
  uptr addr, size;
  cur.GetAccess(&addr, &size, nullptr);

  // Printf("write thr.sid: %u, &this: %p, addr: %u, size: %u\n", thr->fast_state.sid(), this, addr, size);
  for (u8 i = 0; i < size; ++i) {
    HBShadow* hb_shadow = shadow(addr+i);
    HBEpoch race = hb_shadow->HandleWrite(thr, cur);
    if (race.raw() != HBEpoch::kEmpty) {
      // Printf("Race!\n");
      return race;
    }
  }

  return HBEpoch(HBEpoch::kEmpty);
}

HBEpoch HBShadow::HandleRead(ThreadState *thr, HBEpoch cur) {
  Sid sid = cur.sid();
  uptr addr, size;
  AccessType typ;
  cur.GetAccess(&addr, &size, &typ);

  HBEpoch old_wx = HBEpoch(LoadHBEpoch(wx_p()));
  HBEpoch old_rx = HBEpoch(LoadHBEpoch(rx_p()));

  // first access
  if (LIKELY(old_wx.raw() == HBEpoch::kEmpty)) {
    // Printf("- old wx is empty\n");
    if (!(typ & kAccessCheckOnly)) {
      StoreHBEpoch(rx_p(), cur.raw());   // update the read epoch
      // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    }
    return HBEpoch(HBEpoch::kEmpty);
  }

  // same thread accessing, only update the epoch if the access type is weaker
  if (LIKELY(old_wx.sid() == sid)) {
    // Printf("- old wx is same sid\n");
    if (!(typ & kAccessCheckOnly) && old_rx.IsWeakerOrEqual(typ)) {
      StoreHBEpoch(rx_p(), cur.raw());   // update the read epoch
      // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    }
    return HBEpoch(HBEpoch::kEmpty);
  }

  // if both are atomic then not a race
  if (LIKELY(old_wx.IsBothAtomic(typ))) {
    // Printf("- old wx is also atomic\n");
    StoreHBEpoch(rx_p(), cur.raw());     // update the read epoch
    // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    return HBEpoch(HBEpoch::kEmpty);
  }

  // if HB-ordered then not a race
  if (LIKELY(thr->clock.Get(old_wx.sid()) >= old_wx.epoch())) {
    // Printf("- old wx is hb ordered\n");
    // Printf("- old sid: %u, old: %u, cur sid: %u, cur: %u\n", old_wx.sid(), old_wx.epoch(), sid, thr->clock.Get(old_wx.sid()));
    StoreHBEpoch(rx_p(), cur.raw());   // update the read epoch
    Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, cur.epoch());
    return HBEpoch(HBEpoch::kEmpty);
  }
  Printf("Race r with w!\n");
  return old_wx;
}

HBEpoch HBShadow::HandleWrite(ThreadState *thr, HBEpoch cur) {
  // Printf("Handling write\n");
  Sid sid = cur.sid();
  uptr addr, size;
  AccessType typ;
  cur.GetAccess(&addr, &size, &typ);

  HBEpoch old_wx = HBEpoch(LoadHBEpoch(wx_p()));
  HBEpoch old_rx = HBEpoch(LoadHBEpoch(rx_p()));

  bool is_w_race = true;
  bool is_r_race = true;

  // first access
  if (LIKELY(old_wx.raw() == HBEpoch::kEmpty)) {
    // Printf("- old wx is empty\n");
    if (!(typ & kAccessCheckOnly)) {
      StoreHBEpoch(wx_p(), cur.raw());   // update the read epoch
      // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    }
    is_w_race = false;
  }
  if (old_rx.raw() == HBEpoch::kEmpty) {
    // Printf("- old rx is empty\n");
    if (!(typ & kAccessCheckOnly)) {
      StoreHBEpoch(wx_p(), cur.raw());   // update the read epoch
      // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    }
    is_r_race = false;
  }
  if (!(is_w_race || is_r_race)) return HBEpoch(HBEpoch::kEmpty);

  // same thread accessing, only update the epoch if the access type is weaker
  if (LIKELY(old_wx.sid() == sid)) {
    // Printf("- old wx has same sid\n");
    if (!(typ & kAccessCheckOnly) && old_wx.IsWeakerOrEqual(typ)) {
      StoreHBEpoch(wx_p(), cur.raw());   // update the read epoch
      // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    }
    is_w_race = false;
  }
  if (LIKELY(old_rx.sid() == sid)) {
    // Printf("- old rx has same sid\n");
    // if (!(typ & kAccessCheckOnly) && old_rx.IsWeakerOrEqual(typ)) {
    //   StoreHBEpoch(wx_p(), cur.raw());   // update the read epoch
    //   // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    // }
    is_r_race = false;
  }
  if (!(is_w_race || is_r_race)) return HBEpoch(HBEpoch::kEmpty);

  // if both are atomic then not a race
  if (LIKELY(old_wx.IsBothAtomic(typ))) {
    // Printf("- old wx is atomic\n");
    StoreHBEpoch(wx_p(), cur.raw());     // update the read epoch
    // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    is_w_race = false;
  }
  if (LIKELY(old_rx.IsBothAtomic(typ))) {
    // Printf("Both are atomic\n");
    // Printf("- old rx is atomic\n");
    StoreHBEpoch(wx_p(), cur.raw());     // update the read epoch
    // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    is_r_race = false;
  }
  if (!(is_w_race || is_r_race)) return HBEpoch(HBEpoch::kEmpty);

  // if HB-ordered then not a race
  if (LIKELY(thr->clock.Get(old_wx.sid()) >= old_wx.epoch())) {
    // Printf("- old wx is hb ordered\n");
    // Printf("- old sid: %u, old: %u, cur sid: %u, cur: %u\n", old_wx.sid(), old_wx.epoch(), sid, thr->clock.Get(old_wx.sid()));
    StoreHBEpoch(wx_p(), cur.raw());   // update the read epoch
    // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    // HBEpoch test = HBEpoch(LoadHBEpoch(wx_p()));
    // Printf("- stored then loaded - sid: %u, epoch: %u\n", test.sid(), test.epoch());
    is_w_race = false;
  }
  else {
    Printf("Race w with w!\n");
    return old_wx;
  }

  if (LIKELY(thr->clock.Get(old_rx.sid()) >= old_rx.epoch())) {
    // Printf("- old rx is hb ordered\n");
    // Printf("- old sid: %u, old: %u, cur sid: %u, cur: %u\n", old_rx.sid(), old_rx.epoch(), sid, thr->clock.Get(old_rx.sid()));
    StoreHBEpoch(wx_p(), cur.raw());     // update the read epoch
    // Printf("- store hb epoch - sid: %u, epoch: %u\n", sid, epoch);
    is_r_race = false;
  }
  else {
    // Printf("- there is race with old rx\n");
    Printf("Race w with r!\n");
    return old_rx;
  }

  return HBEpoch(HBEpoch::kEmpty);
}
}  // namespace __psan