//===-- tsan_vector_clock.cpp ---------------------------------------------===//
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
#include "tsan_sync_clock.h"

#include "sanitizer_common/sanitizer_placement_new.h"
#include "tsan_mman.h"
#include "tsan_rtl.h"

namespace __tsan {
SyncClock::SyncClock() {
    clock_ = nullptr;
    u_ = kEpochZero;
    last_release_was_store_ = true;
    last_release_was_atomic_ = false;
}

SyncClock::~SyncClock() {
  clock_->DropRef();
}
}