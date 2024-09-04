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

#include "../psan_mman.h"
#include "../psan_platform.h"

#include "sanitizer_common/sanitizer_mutex.h"

namespace __psan {

enum class RawHBEpoch : u32 {};

class HBEpoch {
public:
  static constexpr RawHBEpoch kEmpty = static_cast<RawHBEpoch>(0);

  HBEpoch(FastState state, u32 addr, u32 size, AccessType typ) {
    raw_ = state.raw_;
    DCHECK_GT(size, 0);
    DCHECK_LE(size, 8);
    UNUSED Sid sid0 = part_.sid_;
    UNUSED u16 epoch0 = part_.epoch_;
    raw_ |= (!!(typ & kAccessAtomic) << kIsAtomicShift) |
            (!!(typ & kAccessRead) << kIsReadShift) |
            (((((1u << size) - 1) << (addr & 0x7)) & 0xff) << kAccessShift);
    // Note: we don't check kAccessAtomic because it overlaps with
    // FastState::ignore_accesses_ and it may be set spuriously.
    DCHECK_EQ(part_.is_read_, !!(typ & kAccessRead));
    DCHECK_EQ(sid(), sid0);
    DCHECK_EQ(epoch(), epoch0);
  }

  explicit HBEpoch(RawHBEpoch x = HBEpoch::kEmpty) { raw_ = static_cast<u32>(x); }
  // ALWAYS_INLINE HBEpoch(Sid sid, Epoch epoch) {
  //   part_.sid_ = sid;
  //   part_.epoch_ = static_cast<u16>(epoch);
  // }

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
        (part_.is_atomic_ >= is_atomic);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const u32 kAtomicMask = (1 << kIsAtomicShift);
    bool res = (raw_ & kAtomicMask) >=
               ((is_atomic << kIsAtomicShift));

    DCHECK_EQ(res, res0);
    return res;
#else
    return res0;
#endif
  }

  void Set(Sid sid, Epoch epoch) {
    part_.sid_ = sid;
    part_.epoch_ = static_cast<u16>(epoch);
  }

  // The FreedMarker must not pass "the same access check" so that we don't
  // return from the race detection algorithm early.
  static RawHBEpoch FreedMarker() {
    FastState fs;
    fs.SetSid(kFreeSid);
    fs.SetEpoch(kEpochLast);
    HBEpoch s(fs, 0, 8, kAccessWrite);
    return s.raw();
  }

  static RawHBEpoch FreedInfo(Sid sid, Epoch epoch) {
    HBEpoch s;
    s.part_.sid_ = sid;
    s.part_.epoch_ = static_cast<u16>(epoch);
    s.part_.access_ = kFreeAccess;
    return s.raw();
  }

  static RawHBEpoch ReadSharedMarker() {
    FastState fs;
    fs.SetSid(kFreeSid);
    fs.SetEpoch(kEpochLast);
    HBEpoch s(fs, 0, 0, kAccessRead);
    return s.raw();
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

 public:
  // .rodata shadow marker, see MapRodata and ContainsSameAccessFast.
  static constexpr RawHBEpoch kRodata =
      static_cast<RawHBEpoch>(1 << kIsReadShift);
};

ALWAYS_INLINE RawHBEpoch LoadHBEpoch(RawHBEpoch* p) {
  return static_cast<RawHBEpoch>(
      atomic_load((atomic_uint32_t *)p, memory_order_relaxed));
}

ALWAYS_INLINE void StoreHBEpoch(RawHBEpoch *hp, RawHBEpoch h) {
  atomic_store((atomic_uint32_t *)hp, static_cast<u32>(h),
               memory_order_relaxed);
}

class HBShadow {
public:
  HBEpoch HandleRead(ThreadState *thr, HBEpoch cur);
  HBEpoch HandleWrite(ThreadState *thr, HBEpoch cur);

  HBEpoch wx() const { return wx_; };
  HBEpoch rx() const { return rx_; };

  RawHBEpoch* wx_p() { return (RawHBEpoch*) &wx_; }
  RawHBEpoch* rx_p() { return (RawHBEpoch*) &rx_; }

  void SetWx(HBEpoch wx) { wx_ = wx; }
  void SetRx(HBEpoch rx) { rx_ = rx; }

  void SetWx(FastState state, u32 addr, u32 size, AccessType typ) { wx_ = HBEpoch(state, addr, size, typ); }
  void SetRx(FastState state, u32 addr, u32 size, AccessType typ) { rx_ = HBEpoch(state, addr, size, typ); }
  void SetWx(RawHBEpoch x) { wx_ = HBEpoch(x); }
  void SetRx(RawHBEpoch x) { rx_ = HBEpoch(x); }

  void TransitionToReadShared();
  HBEpoch SetRv(HBEpoch cur);

private:
  HBEpoch wx_;
  HBEpoch wxa_;
  HBEpoch rx_;
  HBEpoch rxa_;
  HBEpoch rv_[kThreadSlotCount];
  HBEpoch rva_[kThreadSlotCount];
};

class HBShadowCell {
public:
  HBShadow* shadow(u8 i) { return &shadows_[i]; }

  HBEpoch HandleRead(ThreadState *thr, HBEpoch cur);
  HBEpoch HandleWrite(ThreadState *thr, HBEpoch cur);
private:
  HBShadow shadows_[kShadowCell];
};

class Shadow {
public:
  static constexpr RawShadow kEmpty = static_cast<RawShadow>(0);

  HBShadowCell* subshadow() { return subshadow_; }
  RawShadow raw() const { return static_cast<RawShadow>(raw_); }

  explicit Shadow(HBShadowCell* hbsh) { subshadow_ = hbsh; }
  explicit Shadow(RawShadow x = Shadow::kEmpty) { raw_ = static_cast<u64>(x); }

  static Shadow MakeHBShadowCell();

private:
  union {
    HBShadowCell* subshadow_;
    u64 raw_;
  };
  StaticSpinMutex mtx;

 public:
  // .rodata shadow marker, see MapRodata and ContainsSameAccessFast.
  static constexpr RawShadow kRodata = static_cast<RawShadow>(1);
};

// ALWAYS_INLINE RawShadow LoadShadow(RawShadow *p) {
//   return static_cast<RawShadow>(
//       atomic_load((atomic_uint64_t *)p, memory_order_relaxed));
// }

// ALWAYS_INLINE void StoreShadow(RawShadow *sp, RawShadow s) {
//   atomic_store((atomic_uint64_t *)sp, static_cast<u64>(s),
//                memory_order_relaxed);
// }

ALWAYS_INLINE HBShadowCell* LoadHBShadowCell(RawShadow *p) {
  RawShadow shadow = static_cast<RawShadow>(
      atomic_load((atomic_uint64_t *)p, memory_order_relaxed));
  if (LIKELY(shadow != Shadow::kEmpty)) return Shadow(shadow).subshadow();

  // If there is no HBShadow, make a new one
  // slow case, only needs to happen once per variable
  Shadow newsh = Shadow::MakeHBShadowCell();
  atomic_store((atomic_uint64_t *)p, static_cast<u64>(newsh.raw()), memory_order_release);

  RawShadow other_newsh_raw = static_cast<RawShadow>(atomic_load((atomic_uint64_t *)p, memory_order_acquire));
  if (other_newsh_raw != newsh.raw()) {
    Printf("Free HBShadowCell because it was allocated concurrently.\n");
    FreeImpl(newsh.subshadow());
  }
  // Printf("Allocated: %p!\n", newsh.raw());

  Shadow other_newsh = Shadow(other_newsh_raw);
  return other_newsh.subshadow();
}

ALWAYS_INLINE RawShadow LoadRawShadowFromUserAddress(uptr p) {
  RawShadow* rawp = MemToShadow(p);
  RawShadow raw = static_cast<RawShadow>(atomic_load((atomic_uint64_t *)rawp, memory_order_relaxed));
  return raw;
}

}  // namespace __psan

#endif