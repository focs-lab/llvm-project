//===-- tsan_sync.h ---------------------------------------------*- C++ -*-===//
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
#ifndef TSAN_VAR_H
#define TSAN_VAR_H

#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_mutex.h"
#include "sanitizer_common/sanitizer_placement_new.h"
#include "tsan_defs.h"
#include "tsan_ilist.h"
#include "tsan_vector_clock.h"
#include "tsan_mman.h"

namespace __tsan {

struct WriteEpoch {
  WriteEpoch() : sid(kFreeSid), epoch(kEpochZero) {}
  Sid sid;
  Epoch epoch;
};

struct VarMeta {
  VarMeta() = delete;
  VarMeta(uptr addr) : addr(addr) { rv.Reset(); }

  uptr addr;
  WriteEpoch wx;
  VectorClock rv;
  INode node;
};

typedef IList<VarMeta, &VarMeta::node> VarMetaList;

ALWAYS_INLINE VarMeta* FindVarMeta(VarMetaList &vmlist, uptr addr) {
  VarMeta* vm = vmlist.Front();
  while (vm != nullptr) {
    if (vm->addr == addr) return vm;
    vm = vmlist.Prev(vm);
  }
  return vm;
}

}  // namespace __tsan

#endif  // TSAN_VAR_H
