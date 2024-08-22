//===-- psan_flags.h --------------------------------------------*- C++ -*-===//
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
// NOTE: This file may be included into user code.
//===----------------------------------------------------------------------===//

#ifndef PSAN_FLAGS_H
#define PSAN_FLAGS_H

#include "sanitizer_common/sanitizer_flags.h"
#include "sanitizer_common/sanitizer_deadlock_detector_interface.h"

namespace __psan {

struct Flags : DDFlags {
#define PSAN_FLAG(Type, Name, DefaultValue, Description) Type Name;
#include "psan_flags.inc"
#undef PSAN_FLAG

  void SetDefaults();
  void ParseFromString(const char *str);
};

void InitializeFlags(Flags *flags, const char *env,
                     const char *env_option_name = nullptr);
}  // namespace __psan

#endif  // PSAN_FLAGS_H
