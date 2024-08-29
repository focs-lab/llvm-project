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
#include "psan_rtl.h"
#include "psan_hb.h"
#include "../psan_mman.h"

namespace __psan {
bool HBShadow::HandleRead(ThreadState *thr, RawShadow* shadow_mem, SubShadow cur) {
  Sid sid = cur.sid();
  Epoch epoch = cur.epoch();
  uptr addr, size;
  AccessType typ;
  cur.GetAccess(&addr, &size, &typ);

  Shadow shadow(LoadShadow(shadow_mem));
  HBShadowCell* hb_shadow_cell = shadow.subshadow();

  bool has_race = false;

  for (u8 i = 0; i < size; ++i) {
    has_race |= HandleReadSingle(thr, hb_shadow_cell->shadow(addr+i), sid, epoch, typ);
  }

  return has_race;
}

ALWAYS_INLINE bool HandleReadSingle(ThreadState *thr, HBShadow* hb_shadow, Sid sid, Epoch epoch, AccessType typ) {
  RawHBEpoch* wx_p = hb_shadow->wx_p();
  RawHBEpoch* rx_p = hb_shadow->rx_p();
  HBEpoch old_wx = HBEpoch(LoadHBEpoch(wx_p));
  HBEpoch old_rx = HBEpoch(LoadHBEpoch(rx_p));

  // first access
  if (LIKELY(old_wx.sid() == kFreeSid)) {
    if (!(typ & kAccessCheckOnly)) {
      StoreHBEpoch(rx_p, HBEpoch(sid, epoch).raw());   // update the read epoch
      return false;
    }
  }

  // same thread accessing, only update the epoch if the access type is weaker
  if (LIKELY(old_wx.sid() == sid)) {
    if (!(typ & kAccessCheckOnly) && old_rx.IsWeakerOrEqual(typ)) {
      StoreHBEpoch(rx_p, HBEpoch(sid, epoch).raw());   // update the read epoch
      return false;
    }
  }

  // if both are atomic then not a race
  if (LIKELY(old_wx.IsBothAtomic(typ))){
    StoreHBEpoch(rx_p, HBEpoch(sid, epoch).raw());     // update the read epoch
    return false;
  }

  // if HB-ordered then not a race
  if (LIKELY(thr->clock.Get(old_wx.sid()) >= old_wx.epoch())) {
      StoreHBEpoch(rx_p, HBEpoch(sid, epoch).raw());   // update the read epoch
    return false;
  }
  return true;
}

bool HBShadow::HandleWrite(ThreadState *thr, RawShadow* shadow_mem, SubShadow cur) {
  Sid sid = cur.sid();
  Epoch epoch = cur.epoch();
  uptr addr, size;
  AccessType typ;
  cur.GetAccess(&addr, &size, &typ);

  Shadow shadow(LoadShadow(shadow_mem));
  HBShadowCell* hb_shadow_cell = shadow.subshadow();

  bool has_race = false;

  for (u8 i = 0; i < size; ++i) {
    has_race |= HandleWriteSingle(thr, hb_shadow_cell->shadow(addr+i), sid, epoch, typ);
  }

  return has_race;
}

ALWAYS_INLINE bool HandleWriteSingle(ThreadState *thr, HBShadow* hb_shadow, Sid sid, Epoch epoch, AccessType typ) {
  RawHBEpoch* wx_p = hb_shadow->wx_p();
  RawHBEpoch* rx_p = hb_shadow->rx_p();
  HBEpoch old_wx = HBEpoch(LoadHBEpoch(wx_p));
  HBEpoch old_rx = HBEpoch(LoadHBEpoch(rx_p));

  bool is_w_race = false;
  bool is_r_race = false;

  // first access
  if (LIKELY(old_wx.sid() == kFreeSid)) {
    if (!(typ & kAccessCheckOnly)) {
      StoreHBEpoch(wx_p, HBEpoch(sid, epoch).raw());   // update the read epoch
    is_w_race = false;
  }
  if (old_rx.sid() == kFreeSid) {
    is_r_race = false;
  }
  if (!(is_w_race || is_r_race)) return false;

  // same thread accessing, only update the epoch if the access type is weaker
  if (LIKELY(old_wx.sid() == sid)) {
    if (!(typ & kAccessCheckOnly) && old_rx.IsWeakerOrEqual(typ)) {
      StoreHBEpoch(wx_p, HBEpoch(sid, epoch).raw());   // update the read epoch
    is_w_race = false;
  }
  if (LIKELY(old_rx.sid() == sid)) {
    is_r_race = false;
  }
  if (!(is_w_race || is_r_race)) return false;

  // if both are atomic then not a race
  if (LIKELY(old_wx.IsBothAtomic(typ))){
    StoreHBEpoch(wx_p, HBEpoch(sid, epoch).raw());     // update the read epoch
    is_w_race = false;
  }
  if (LIKELY(old_rx.IsBothAtomic(typ))){
    is_r_race = false;
  }
  if (!(is_w_race || is_r_race)) return false;

  // if HB-ordered then not a race
  if (LIKELY(thr->clock.Get(old_wx.sid()) >= old_wx.epoch())) {
      StoreHBEpoch(wx_p, HBEpoch(sid, epoch).raw());   // update the read epoch
    is_w_race = false;
  }
  if (LIKELY(thr->clock.Get(old_rx.sid()) >= old_rx.epoch())) {
    is_r_race = false;
  }
  return is_w_race || is_r_race;
}
}  // namespace __psan