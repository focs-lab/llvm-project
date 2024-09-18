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
#include "tsan_mman.h"
#include "tsan_vector_clock.h"

namespace __tsan {

struct WriteEpoch {
  WriteEpoch() : sid(kFreeSid), epoch(kEpochZero) {}
  WriteEpoch(Sid sid, Epoch epoch) : sid(sid), epoch(epoch) {}
  Sid sid;
  Epoch epoch;
};

struct VarMeta {
  VarMeta() { rv.Reset(); }
  void Reset() { wx = WriteEpoch(); }

  WriteEpoch wx;
  VectorClock rv;
};

struct VarMetaNode {
  static const u16 kEmpty = 0;
  static const u16 kBlack = 0, kRed = 1;

  void Init(uptr a, u16 p);

  uptr addr;
  u16 left, right, parent;
  u16 color;  // dont actually need 2 bytes but padding anyway
  // VarMeta vm;
};

struct VarMetaZone {
  static constexpr uptr kMaxVmsPerZone = 0x100000;
  u32 vmis[kMaxVmsPerZone];
};

// RB tree https://www.geeksforgeeks.org/introduction-to-red-black-tree/
class VarMetaSet {
 public:
  static constexpr u16 kFirstNode = 1;
  static constexpr u16 kMaxNodes = 2048;
  static constexpr uptr kShift = 3;
  static constexpr uptr kAddrMask1 = (~(VarMetaZone::kMaxVmsPerZone-1)) << kShift;
  static constexpr uptr kAddrMask2 = ~kAddrMask1;
  static constexpr uptr kMaxVms = Min((unsigned long) (1<<21), kMaxNodes * VarMetaZone::kMaxVmsPerZone);

  static VarMetaSet* Alloc() {
    // mmap will return zero-initialized memory
    VarMetaSet* vmset = (VarMetaSet*)MmapNoReserveOrDie(sizeof(VarMetaSet), "VarMetaSet");

    // internal_memset(vmset->zones_, 0, sizeof(vmset->zones_));

    return vmset;
  }
  static void Free(VarMetaSet* vmset) { UnmapOrDie(vmset, sizeof(VarMetaSet)); }

  u16 node_count() const { return node_count_; }
  VarMeta* Find(uptr addr) {
    uptr addr_hi = addr & kAddrMask1;
    uptr addr_lo = (addr & kAddrMask2) >> kShift;
    if (node_count_ > 0) {
      u16 lb = LowerBound(addr_hi);
      if (nodes_[lb].addr == addr_hi)
        return &vms_[zones_[lb].vmis[addr_lo]];
      else
        return nullptr;
    }

    return nullptr;
  }

  VarMeta* FindOrCreate(uptr addr) {
    PREFETCH(&nodes_[root_]);
    accesses_++;
    uptr addr_hi = addr & kAddrMask1;
    uptr addr_lo = (addr & kAddrMask2) >> kShift;
    // Printf("addr=%p (addr & kAddrMask2)=%p (addr & kAddrMask2) >> kShift = %p\n", addr, (addr & kAddrMask2), addr_lo);
    if (node_count_ > 0) {
      u16 lb = LowerBound(addr_hi);
      if (nodes_[lb].addr == addr_hi) {
        u16 idx = zones_[lb].vmis[addr_lo];
        if (UNLIKELY(idx == 0)) {
          idx = zones_[lb].vmis[addr_lo] = ++vm_count_;
          // Printf("idx=%p\n", idx);
          vms_[idx].Reset();
        }
        CHECK_LE(idx, vm_count_);
        return &vms_[idx];
      }
      // else if (tid <= 1)
      //   return &nodes_[lb];
      else
        return Create(lb, addr);
    }

    // first node in the tree
    node_count_ = 1;
    vm_count_ = 1;
    nodes_[kFirstNode].Init(addr_hi, VarMetaNode::kEmpty);
    zones_[kFirstNode].vmis[addr_lo] = vm_count_;
    vms_[vm_count_].Reset();
    root_ = kFirstNode;
    return &vms_[kFirstNode];
  }

  ~VarMetaSet() {
    Printf("vmset vmcount = %u\n", vm_count_);
  }

  Tid tid;

 private:
  u64 accesses_ = 0, inserts_ = 0;
  u16 node_count_ = 0, root_;
  u32 vm_count_ = 0;
  VarMetaNode nodes_[kMaxNodes];
  VarMetaZone zones_[kMaxNodes];
  VarMeta vms_[kMaxVms];

  NOINLINE u16 LowerBound(uptr addr) {
    CHECK_GE(node_count_, 1);

    u16 parent = VarMetaNode::kEmpty, curr = root_;
    while (curr != VarMetaNode::kEmpty) {
      parent = curr;
      VarMetaNode& node = nodes_[curr];
      PREFETCH(&nodes_[node.left]);
      PREFETCH(&nodes_[node.right]);
      // PREFETCH(&zones_[curr].vmis[(addr & kAddrMask2) >> kShift]);
      if (LIKELY(addr == node.addr))
        return curr;
      else if (addr < node.addr)
        curr = node.left;
      else
        curr = node.right;
    }

    return parent;
  }

  NOINLINE VarMeta* Create(u16 parent, uptr addr) {
    CHECK_NE(parent, VarMetaNode::kEmpty);
    CHECK_NE(node_count_, kMaxNodes);
    CHECK_NE(vm_count_, kMaxVms);

    uptr addr_hi = addr & kAddrMask1;
    uptr addr_lo = (addr & kAddrMask2) >> kShift;

    inserts_++;
    // Printf("%u: %llu/%llu\n", tid, inserts_, accesses_);

    u16 new_pos = ++node_count_;
    VarMetaNode& np = nodes_[parent];
    if (addr_hi < np.addr)
      np.left = new_pos;
    else
      np.right = new_pos;

    nodes_[new_pos].Init(addr_hi, parent);
    zones_[new_pos].vmis[addr_lo] = ++vm_count_;
    vms_[vm_count_].Reset();
    if (np.parent != VarMetaNode::kEmpty) FixInsert(new_pos);

    return &vms_[new_pos];
  }

  void LeftRotate(u16 x) {
    VarMetaNode& nx = nodes_[x];
    u16 y = nx.right;
    VarMetaNode& ny = nodes_[y];
    nx.right = ny.left;

    if (ny.left != VarMetaNode::kEmpty)
      nodes_[ny.left].parent = x;

    ny.parent = nx.parent;
    VarMetaNode& nxp = nodes_[nx.parent];
    if (nx.parent == VarMetaNode::kEmpty)
      root_ = y;
    else if (x == nxp.left)
      nxp.left = y;
    else
      nxp.right = y;

    ny.left = x;
    nx.parent = y;
  }

  void RightRotate(u16 x) {
    VarMetaNode& nx = nodes_[x];
    u16 y = nx.left;
    VarMetaNode& ny = nodes_[y];
    nx.left = ny.right;

    if (ny.right != VarMetaNode::kEmpty)
      nodes_[ny.right].parent = x;

    ny.parent = nx.parent;
    VarMetaNode& nxp = nodes_[nx.parent];
    if (nx.parent == VarMetaNode::kEmpty)
      root_ = y;
    else if (x == nxp.right)
      nxp.right = y;
    else
      nxp.left = y;

    ny.right = x;
    nx.parent = y;
  }

  NOINLINE void FixInsert(u16 k) {
    VarMetaNode* nk = &nodes_[k];
    while (k != root_ && nk->color == VarMetaNode::kRed) {
      VarMetaNode* nkp = &nodes_[nk->parent];
      VarMetaNode* nkpp = &nodes_[nkp->parent];

      if (nk->parent == nkpp->left) {
        u16 u = nkpp->right;  // uncle
        VarMetaNode* nu = &nodes_[u];

        if (nu->color == VarMetaNode::kRed) {
          nkp->color = VarMetaNode::kBlack;
          nu->color = VarMetaNode::kBlack;
          nkpp->color = VarMetaNode::kRed;
          k = nkp->parent;
        }
        else {
          if (k == nkp->right) {
            k = nk->parent;
            LeftRotate(k);
            // k has been reassigned and rotated
            nk = &nodes_[k];
            nkp = &nodes_[nk->parent];
            nkpp = &nodes_[nkp->parent];
          }
          // k may have been reassigned
          nkp->color = VarMetaNode::kBlack;
          nkpp->color = VarMetaNode::kRed;
          RightRotate(nkp->parent);
        }
      }
      else {
        u16 u = nkpp->left;  // uncle
        VarMetaNode* nu = &nodes_[u];

        if (nu->color == VarMetaNode::kRed) {
          nkp->color = VarMetaNode::kBlack;
          nu->color = VarMetaNode::kBlack;
          nkpp->color = VarMetaNode::kRed;
          k = nkp->parent;
        }
        else {
          if (k == nkp->left) {
            k = nk->parent;
            RightRotate(k);
            // k has been reassigned and rotated
            nk = &nodes_[k];
            nkp = &nodes_[nk->parent];
            nkpp = &nodes_[nkp->parent];
          }
          nkp->color = VarMetaNode::kBlack;
          nkpp->color = VarMetaNode::kRed;
          LeftRotate(nkp->parent);
        }
      }
    }
    nodes_[root_].color = VarMetaNode::kBlack;
  }
};

static constexpr u64 kVarMetaSetSize = sizeof(VarMetaSet);

}  // namespace __tsan

#endif  // TSAN_VAR_H
