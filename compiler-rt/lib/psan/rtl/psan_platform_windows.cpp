//===-- psan_platform_windows.cpp -----------------------------------------===//
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
// Windows-specific code.
//===----------------------------------------------------------------------===//

#include "sanitizer_common/sanitizer_platform.h"
#if SANITIZER_WINDOWS

#include "psan_platform.h"

#include <stdlib.h>

namespace __psan {

void WriteMemoryProfile(char *buf, uptr buf_size, u64 uptime_ns) {}

void InitializePlatformEarly() {
}

void InitializePlatform() {
}

}  // namespace __psan

#endif  // SANITIZER_WINDOWS
