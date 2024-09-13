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

namespace __tsan {
void VarMetaNode::Init(uptr a, u16 p) {
  addr = a;
  parent = p;
  left = kEmpty;
  right = kEmpty;
  color = kBlack;
  vm = New<VarMeta>();
}

}  // namespace __tsan
