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
  Sid sid;
  Epoch epoch;
};

struct VarMeta {
  VarMeta() { rv.Reset(); }

  WriteEpoch wx;
  VectorClock rv;
};

struct VarMetaNode {
  static const u16 kEmpty = 0;
  static const u16 kBlack = 0, kRed = 1;

  void Init(uptr a, u16 p);

  uptr addr;
  u16 parent, left, right;
  u16 color;  // dont actually need 2 bytes but padding anyway
  VarMeta* vm;
};

// RB tree https://www.geeksforgeeks.org/introduction-to-red-black-tree/
class VarMetaSet {
 public:
  static constexpr u16 kFirstNode = 1;
  static constexpr u16 kMaxNodes = 65535;

  static VarMetaSet* Alloc() {
    // mmap will return zero-initialized memory
    return (VarMetaSet*)MmapOrDie(kVarMetaSetSize, "VarMetaSet");
  }
  static void Free(VarMetaSet* vmset) { UnmapOrDie(vmset, kVarMetaSetSize); }

  u16 size() const { return size_; }
  VarMetaNode* FindOrCreate(uptr addr) {
    if (size_ > 0) {
      u16 lb = LowerBound(addr);
      if (nodes_[lb].addr == addr)
        return &nodes_[lb];
      else
        return Create(lb, addr);
    }

    nodes_[kFirstNode].Init(addr, VarMetaNode::kEmpty);
    root_ = kFirstNode;
    return &nodes_[kFirstNode];
  }

 private:
  VarMetaNode nodes_[kMaxNodes];
  u16 size_, root_;

  u16 LowerBound(uptr addr) {
    CHECK_GE(size_, 1);

    u16 parent = VarMetaNode::kEmpty, curr = root_;
    while (curr != VarMetaNode::kEmpty) {
      parent = curr;
      VarMetaNode& node = nodes_[curr];
      if (addr < node.addr)
        curr = node.left;
      else if (addr == node.addr)
        return curr;
      else
        curr = node.right;
    }

    return parent;
  }

  VarMetaNode* Create(u16 parent, uptr addr) {
    CHECK_NE(parent, VarMetaNode::kEmpty);

    u16 new_pos = ++size_;
    VarMetaNode& np = nodes_[parent];
    if (addr < np.addr)
      np.left = new_pos;
    else
      np.right = new_pos;

    nodes_[new_pos].Init(addr, parent);
    if (np.parent == VarMetaNode::kEmpty)
      return;
    FixInsert(new_pos);
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

  void FixInsert(u16 k) {
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

        if (nu->color = VarMetaNode::kRed) {
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
