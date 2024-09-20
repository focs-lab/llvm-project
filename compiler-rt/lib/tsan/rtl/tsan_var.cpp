//===-- tsan_var.cpp ---------------------------------------------===//
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
#include "tsan_var.h"

#include "sanitizer_common/sanitizer_placement_new.h"
#include "tsan_mman.h"
#include "tsan_rtl.h"

namespace __tsan {
void VarMetaNode::Init(uptr a, u16 p) {
  addr = a;
  parent = p;
  left = kEmpty;
  right = kEmpty;
  color = kBlack;
  // vm.Reset();
}

NOINLINE void VarMetaSet::BREAK() {}

bool VarMetaSet::CrossRace(VarMetaSet* other, const VectorClock* vc_this,
                           const VectorClock* vc_other) {
  bool has_race = false;
  bool report_bugs = flags()->report_bugs;

  for (u32 i = 1; i <= vm_count_; ++i) {
    VarMeta& vm = vms_[i];
    VarMeta* ovm = other->Find(vm.addr);
    if (!ovm)
      continue;

    WriteEpoch wx_this = vm.wx;
    WriteEpoch wx_other = ovm->wx;

    bool w_this_not_hb = wx_this.epoch > vc_other->Get(wx_this.sid);
    bool w_other_not_hb = wx_other.epoch > vc_this->Get(wx_other.sid);

    has_race |= w_this_not_hb && w_other_not_hb;
    if (has_race) {
      Printf("RACE!\n");
      break;
    }

    VectorClock& rv_this = vm.rv;
    VectorClock& rv_other = ovm->rv;

    bool read_this_not_hb = false;
    for (u16 i = 0; i < kThreadSlotCount; ++i) {
      read_this_not_hb |=
          rv_this.Get(static_cast<Sid>(i)) > vc_other->Get(static_cast<Sid>(i));
    }

    has_race |= read_this_not_hb && w_other_not_hb;
    if (has_race) {
      Printf("RACE!\n");
      break;
    }

    bool read_other_not_hb = false;
    for (u16 i = 0; i < kThreadSlotCount; ++i) {
      read_other_not_hb |=
          rv_other.Get(static_cast<Sid>(i)) > vc_this->Get(static_cast<Sid>(i));
    }

    has_race |= read_other_not_hb && w_this_not_hb;
    if (has_race) {
      Printf("RACE!\n");
      break;
    }
  }

  return has_race && report_bugs;
}

bool VarMetaSet::Acquire(VarMetaSet* other, const VectorClock* vc_this,
                         const VectorClock* vc_other) {
  if (!other)
    return false;
  num_acquires++;
  bool has_race = CrossRace(other, vc_this, vc_other);

  return has_race;
}

static VarMetaSet* AllocVMSet(VarMetaSet** dstp) {
  if (UNLIKELY(!*dstp))
    *dstp = VarMetaSet::Alloc();
  return *dstp;
}

void VarMetaSet::Release(VarMetaSet** otherp, const VectorClock* vc_this,
                         const VectorClock* vc_other) {
  num_releases++;
  VarMetaSet* other = AllocVMSet(otherp);

  for (u32 i = 0; i < vm_count_; ++i) {
    VarMeta& vm = vms_[i];
    VarMeta& ovm = *other->FindOrCreate(vm.addr);

    WriteEpoch wx_this = vm.wx;
    WriteEpoch wx_other = ovm.wx;
    bool w_this_hb = vc_other && wx_this.epoch <= vc_other->Get(wx_this.sid);
    bool w_other_hb = wx_other.epoch <= vc_this->Get(wx_other.sid);
    if (w_this_hb)
      vm.wx = wx_other;
    else if (w_other_hb)
      ovm.wx = wx_this;

    VectorClock& rv_this = vm.rv;
    VectorClock& rv_other = ovm.rv;
    rv_this.Acquire(&rv_other);
    rv_other = rv_this;
  }
}

ThreadVarMeta::ThreadVarMeta(ThreadState* thr) {
  vmset = thr->vmset;
  vc = thr->clock;
  tid = thr->tid;
}

}  // namespace __tsan
