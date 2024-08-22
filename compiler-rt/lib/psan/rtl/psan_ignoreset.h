//===-- psan_ignoreset.h ----------------------------------------*- C++ -*-===//
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
// IgnoreSet holds a set of stack traces where ignores were enabled.
//===----------------------------------------------------------------------===//
#ifndef PSAN_IGNORESET_H
#define PSAN_IGNORESET_H

#include "psan_defs.h"

namespace __psan {

class IgnoreSet {
 public:
  IgnoreSet();
  void Add(StackID stack_id);
  void Reset() { size_ = 0; }
  uptr Size() const { return size_; }
  StackID At(uptr i) const;

 private:
  static constexpr uptr kMaxSize = 16;
  uptr size_;
  StackID stacks_[kMaxSize];
};

}  // namespace __psan

#endif  // PSAN_IGNORESET_H
