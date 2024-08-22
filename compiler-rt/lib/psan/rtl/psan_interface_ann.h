//===-- psan_interface_ann.h ------------------------------------*- C++ -*-===//
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
// Interface for dynamic annotations.
//===----------------------------------------------------------------------===//
#ifndef PSAN_INTERFACE_ANN_H
#define PSAN_INTERFACE_ANN_H

#include <sanitizer_common/sanitizer_internal_defs.h>

// This header should NOT include any other headers.
// All functions in this header are extern "C" and start with __psan_.

#ifdef __cplusplus
extern "C" {
#endif

SANITIZER_INTERFACE_ATTRIBUTE void __psan_acquire(void *addr);
SANITIZER_INTERFACE_ATTRIBUTE void __psan_release(void *addr);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PSAN_INTERFACE_ANN_H
