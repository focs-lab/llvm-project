//===-- tsan_tree_clock.h -------------------------------------*- C++ -*-===//
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
#ifndef TSAN_TREE_CLOCK_H
#define TSAN_TREE_CLOCK_H

#include "tsan_defs.h"
#include "sanitizer_common/sanitizer_common.h"

namespace __tsan {

// Fixed-size tree clock, used both for threads and sync objects.
class TreeClock {
 public:
  TreeClock();

  Epoch Get(Sid sid) const;
  void Set(Sid sid, Epoch v);

  Sid GetRootSid() const;
  void SetRootSid(Sid sid);

  void Reset();
  void Acquire(const TreeClock* src);
  void Release(TreeClock** dstp) const;
  void ReleaseStore(TreeClock** dstp) const;
  void ReleaseStoreAcquire(TreeClock** dstp);
  void ReleaseAcquire(TreeClock** dstp);

  TreeClock& operator=(const TreeClock& other);

struct Node {
  Sid parent;
  Sid first_child;
  Sid prev;
  Sid next;
};

  Node GetNode(Sid sid) const;

 private:
  Epoch clk_[kThreadSlotCount] VECTOR_ALIGNED;
  Epoch aclk_[kThreadSlotCount] VECTOR_ALIGNED;

  union {
    Node nodes_[kThreadSlotCount] VECTOR_ALIGNED;
    u32  raw_nodes_[kThreadSlotCount] VECTOR_ALIGNED;
  };

  Sid stack_[kThreadSlotCount] VECTOR_ALIGNED;
  Sid root_sid_;
  s16 stack_pos_;

  Node& GetNode(Sid sid);
  u32 GetRawNode(Sid sid) const;

  Epoch GetAclk(Sid sid) const;
  void SetAclk(Sid sid, Epoch v);

  bool IsNodeNull(Sid sid) const;
  void DetachNode(Sid sid);
  void PushChild(Sid parent, Sid child);
  void GetUpdatedNodesJoin(const TreeClock* src, Sid parent, Epoch clk);
  void GetUpdatedNodesCopy(const TreeClock& src, Sid parent, Epoch clk);

  template <bool UpdateSrc> void Join(const TreeClock* src);
};

ALWAYS_INLINE Epoch TreeClock::Get(Sid sid) const {
  return clk_[static_cast<u8>(sid)];
  // Printf("%u: Get %u = %u @ %p\n", root_sid_, sid, v, this);
}

ALWAYS_INLINE void TreeClock::Set(Sid sid, Epoch v) {
  // Printf("%u: Set %u = %u @ %p\n", root_sid_, sid, v, this);
  DCHECK_GE(v, clk_[static_cast<u8>(sid)]);
  clk_[static_cast<u8>(sid)] = v;
}

ALWAYS_INLINE Epoch TreeClock::GetAclk(Sid sid) const {
  return aclk_[static_cast<u8>(sid)];
}

ALWAYS_INLINE void TreeClock::SetAclk(Sid sid, Epoch v) {
  DCHECK_GE(v, aclk_[static_cast<u8>(sid)]);
  aclk_[static_cast<u8>(sid)] = v;
}

ALWAYS_INLINE Sid TreeClock::GetRootSid() const {
  return root_sid_;
}

ALWAYS_INLINE void TreeClock::SetRootSid(Sid sid) {
  root_sid_ = sid;
}

ALWAYS_INLINE TreeClock::Node TreeClock::GetNode(Sid sid) const {
  return nodes_[static_cast<u8>(sid)];
}

ALWAYS_INLINE TreeClock::Node& TreeClock::GetNode(Sid sid) {
  return nodes_[static_cast<u8>(sid)];
}

ALWAYS_INLINE u32 TreeClock::GetRawNode(Sid sid) const {
  return raw_nodes_[static_cast<u8>(sid)];
}

}  // namespace __tsan

#endif  // TSAN_TREE_CLOCK_H
