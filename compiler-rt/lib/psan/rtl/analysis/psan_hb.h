//===-- psan_hb.h -----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//


#ifndef PSAN_HB_H
#define PSAN_HB_H

#include "../psan_defs.h"
#include "../psan_shadow.h"
#include "../psan_vector_clock.h"

namespace __psan {

class HBEpoch {
public:
  RawHBEpoch raw() const { return static_cast<RawHBEpoch>(raw_); }
  Sid sid() const { return part_.sid_; }
  Epoch epoch() const { return static_cast<Epoch>(part_.epoch_); }
  u8 access() const { return part_.access_; }

  void GetAccess(uptr *addr, uptr *size, AccessType *typ) const {
    DCHECK(part_.access_ != 0 || raw_ == static_cast<u32>(SubShadow::kRodata));
    if (addr)
      *addr = part_.access_ ? __builtin_ffs(part_.access_) - 1 : 0;
    if (size)
      *size = part_.access_ == kFreeAccess ? kShadowCell
                                           : __builtin_popcount(part_.access_);
    if (typ) {
      *typ = part_.is_read_ ? kAccessRead : kAccessWrite;
      if (part_.is_atomic_)
        *typ |= kAccessAtomic;
      if (part_.access_ == kFreeAccess)
        *typ |= kAccessFree;
    }
  }

  ALWAYS_INLINE
  bool IsBothAtomic(AccessType typ) const {
    u32 is_atomic = !!(typ & kAccessAtomic);
    bool res =
        raw_ & (is_atomic << kIsAtomicShift);
    DCHECK_EQ(res,
              (part_.is_atomic_ && is_atomic));
    return res;
  }

  ALWAYS_INLINE
  bool IsWeakerOrEqual(AccessType typ) const {
    u32 is_atomic = !!(typ & kAccessAtomic);
    UNUSED u32 res0 =
        (part_.is_atomic_ > is_atomic);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const u32 kAtomicReadMask = (1 << kIsAtomicShift) | (1 << kIsReadShift);
    bool res = (raw_ & kAtomicReadMask) >=
               ((is_atomic << kIsAtomicShift));

    DCHECK_EQ(res, res0);
    return res;
#else
    return res0;
#endif
  }

  ALWAYS_INLINE HBEpoch() {
    part_.sid_ = kFreeSid;
    part_.epoch_ = static_cast<u16>(kEpochZero);
  }

  void Set(Sid sid, Epoch epoch) {
    part_.sid_ = sid;
    part_.epoch_ = static_cast<u16>(epoch);
  }

  explicit HBEpoch(RawHBEpoch x) {
    raw_ = static_cast<u32>(x);
  }

  ALWAYS_INLINE HBEpoch(Sid sid, Epoch epoch) {
    part_.sid_ = sid;
    part_.epoch_ = static_cast<u16>(kEpochZero);
  }
private:
    struct Parts {
      u8 access_;
      Sid sid_;
      u16 epoch_ : kEpochBits;
      u16 is_read_ : 1;
      u16 is_atomic_ : 1;
    };
    union {
        Parts part_;
        u32 raw_;
    };

  static constexpr u8 kFreeAccess = 0x81;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  static constexpr uptr kAccessShift = 0;
  static constexpr uptr kIsReadShift = 30;
  static constexpr uptr kIsAtomicShift = 31;
#else
  static constexpr uptr kAccessShift = 24;
  static constexpr uptr kIsReadShift = 1;
  static constexpr uptr kIsAtomicShift = 0;
#endif
};

class HBShadow {
public:
    bool HandleRead(ThreadState *thr, RawShadow* shadow_mem, SubShadow cur, AccessType typ);
    bool HandleWrite(ThreadState *thr, RawShadow* shadow_mem, SubShadow cur, AccessType typ);

    HBEpoch wx() const { return wx_; };
    HBEpoch rx() const { return rx_; };

    RawHBEpoch* wx_raw_p() const { return (RawHBEpoch*) &wx_raw_; }
    RawHBEpoch* rx_raw_p() const { return (RawHBEpoch*) &rx_raw_; }

    void SetWx(Sid sid, Epoch epoch) { wx_.Set(sid, epoch); }
    void SetRx(Sid sid, Epoch epoch) { rx_.Set(sid, epoch); }

private:
    union {
        HBEpoch wx_;
        u32 wx_raw_;
    };
    union {
        HBEpoch rx_;
        u32 rx_raw_;
    };
};

enum class RawHBEpoch : u32 {};

ALWAYS_INLINE RawHBEpoch LoadHBEpoch(RawHBEpoch* p) {
  return static_cast<RawHBEpoch>(
      atomic_load((atomic_uint32_t *)p, memory_order_relaxed));
}

ALWAYS_INLINE void StoreHBEpoch(RawHBEpoch *hp, RawHBEpoch h) {
  atomic_store((atomic_uint32_t *)hp, static_cast<u32>(h),
               memory_order_relaxed);
}


}  // namespace __psan

#endif