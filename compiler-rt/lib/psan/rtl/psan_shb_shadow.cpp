//===-- psan_shadow.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef PSAN_SHBSHADOW_H
#define PSAN_SHBSHADOW_H

#include "psan_defs.h"
#include "psan_shadow.h"
#include "psan_vector_clock.h"

namespace __psan {

class SHBShadow : public SubShadow {

};

}  // namespace __psan

#endif