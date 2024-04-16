//===-- tsan_tree_clock.cpp ---------------------------------------------===//
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
#include "tsan_tree_clock.h"

#include "sanitizer_common/sanitizer_placement_new.h"
#include "tsan_mman.h"

#if TSAN_COLLECT_STATS
#include "tsan_rtl.h"
#endif

namespace __tsan {

#if TSAN_VECTORIZE
const uptr kTreeClockSize = kThreadSlotCount * sizeof(Epoch) / sizeof(m128);
const uptr kTreeClockSize2 = kThreadSlotCount * sizeof(TreeClock::Node) / sizeof(m128);
#endif

TreeClock::TreeClock() { Reset(); }

void TreeClock::Reset() {
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++)
    clk_[i] = kEpochZero;
#else
  m128 z = _mm_setzero_si128();
  m128* vclk = reinterpret_cast<m128*>(clk_);
  for (uptr i = 0; i < kTreeClockSize; i++) _mm_store_si128(&vclk[i], z);
  m128* vaclk = reinterpret_cast<m128*>(aclk_);
  for (uptr i = 0; i < kTreeClockSize; i++) _mm_store_si128(&vaclk[i], z);

  // m128 ff = _mm_set1_epi8(0xff);
  // m128* vnode = reinterpret_cast<m128*>(nodes_);
  // for (uptr i = 0; i < kTreeClockSize2; i++) _mm_store_si128(&vnode[i], ff);

  for (uptr i = 0; i < kThreadSlotCount; ++i) raw_nodes_[i] = 0xffffffff;

  root_sid_ = kFreeSid;
  stack_pos_ = -1;
#endif
}

ALWAYS_INLINE bool TreeClock::IsNodeNull(Sid sid) const {
  return GetRawNode(sid) == 0xffffffff;
}

ALWAYS_INLINE void TreeClock::DetachNode(Sid sid) {
  Node node = GetNode(sid);
  Node& parent_node = GetNode(node.parent);     // get reference because may update the first child

  // if it's the first child, detach from the parent
  if (parent_node.first_child == sid)
    parent_node.first_child = node.next;
  // if it's not the first child, detach from the left sibling
  else
    GetNode(node.prev).next = node.next;

  // also detach from the right sibling
  if (node.next != kFreeSid) {
    GetNode(node.next).prev = node.prev;
  }
}

ALWAYS_INLINE void TreeClock::PushChild(Sid parent, Sid child) {
  // Printf("%u: PushChild %u -> %u\n", root_sid_, parent, child);

  Node parent_node = GetNode(parent);
  Sid parent_first_child = parent_node.first_child;

  // if the parent has existing children, make the new child the left sibling of the current first child
  if (parent_first_child != kFreeSid) {
    GetNode(parent_first_child).prev = child;
  }

  // update the links for the new child
  Node& new_node = GetNode(child);
  new_node.prev = kFreeSid;
  new_node.next = parent_first_child;
  new_node.parent = parent;

  // update the parent's first child to be the new child
  GetNode(parent).first_child = child;
  // Printf("           %u -> %u\n", parent, GetNode(parent).first_child);
}

ALWAYS_INLINE void TreeClock::GetUpdatedNodesJoin(const TreeClock* src, Sid parent, Epoch parent_clk) {
  // Printf("%u: GetUpdatedNodesJoin %u %u %u\n", root_sid_, src->root_sid_, parent, parent_clk);
  Node parent_node = src->GetNode(parent);
  Sid cur_node_sid = parent_node.first_child;

  while (cur_node_sid != kFreeSid) {
    Epoch src_clk = src->Get(cur_node_sid);
    Epoch this_clk = Get(cur_node_sid);
    if (this_clk < src_clk)
      stack_[++stack_pos_] = cur_node_sid;
    else if (src->GetAclk(cur_node_sid) <= parent_clk) break;

    cur_node_sid = src->GetNode(cur_node_sid).next;
  }
}

template <bool UpdateSrc>
ALWAYS_INLINE void TreeClock::Join(const TreeClock* src) {

}

void TreeClock::Acquire(const TreeClock* src) {
  if (!src)
    return;
  // Printf("%u: Acquire %u @ %p - %p\n", static_cast<u8>(root_sid_), src->root_sid_, this, src);
  // Printf("    %x\n", nodes_[0]);
  // Printf("this:       [%u] %u (%u) %u (%u) %u (%u)\n", root_sid_
  //                                   , clk_[1], aclk_[1]
  //                                   , clk_[2], aclk_[2]
  //                                   , clk_[3], aclk_[3]);
  // Printf("src:        [%u] %u (%u) %u (%u) %u (%u)\n", src->root_sid_
  //                                   , src->clk_[1], src->aclk_[1]
  //                                   , src->clk_[2], src->aclk_[1]
  //                                   , src->clk_[3], src->aclk_[1]);
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++)
    clk_[i] = max(clk_[i], src->clk_[i]);
#else
  // m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
  // m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(src->clk_);
  // for (uptr i = 0; i < kTreeClockSize; i++) {
  //   m128 s = _mm_load_si128(&vsrc[i]);
  //   m128 d = _mm_load_si128(&vdst[i]);
  //   m128 m = _mm_max_epu16(s, d);
  //   _mm_store_si128(&vdst[i], m);
  // }

  // return;

  if (root_sid_ == kFreeSid) {
    *this = *src;
    return;
  }

  Sid src_root_sid = src->root_sid_;            // the root sid of the src clock

  Epoch src_src_root_clk = src->Get(src_root_sid);   // the epoch of the root sid in the src clock
  Epoch this_src_root_clk = Get(src_root_sid);       // the epoch of the root sid in this clock

  if (src_src_root_clk <= this_src_root_clk) return;      // return if the src clock is older or same
  if (src_root_sid != root_sid_ && !IsNodeNull(src_root_sid)) DetachNode(src_root_sid);

  // clock assignments
  Set(src_root_sid, src_src_root_clk);               // update epoch
  SetAclk(src_root_sid, Get(root_sid_));        // to indicate that the root sid was updated at this epoch

  PushChild(root_sid_, src_root_sid);
  GetUpdatedNodesJoin(src, src_root_sid, this_src_root_clk);

  // only update the clocks that matter
  // Printf("stack: ");
  while (stack_pos_ >= 0) {
    Sid cur_node_sid = stack_[stack_pos_--];
    // Printf("%u ", cur_node_sid);
    Epoch cur_node_clock = Get(cur_node_sid);

    // need to reorder this node in the tree
    if (!IsNodeNull(cur_node_sid)) DetachNode(cur_node_sid);

    Set(cur_node_sid, src->Get(cur_node_sid));
    SetAclk(cur_node_sid, src->GetAclk(cur_node_sid));

    PushChild(src->GetNode(cur_node_sid).parent, cur_node_sid);
    GetUpdatedNodesJoin(src, cur_node_sid, cur_node_clock);
  }
  // Printf("\n");

  // Printf("res:       [%u] %u (%u) %u (%u) %u (%u)\n", root_sid_
  //                                   , clk_[1], aclk_[1]
  //                                   , clk_[2], aclk_[2]
  //                                   , clk_[3], aclk_[3]);
  
#endif
}

static TreeClock* AllocClock(TreeClock** dstp) {
  if (UNLIKELY(!*dstp))
    *dstp = New<TreeClock>();
  return *dstp;
}

void TreeClock::Release(TreeClock** dstp) const {
  TreeClock* dst = AllocClock(dstp);
  // Printf("%u: Release %u @ %p\n", static_cast<u8>(root_sid_), dst->root_sid_, this);
  dst->Acquire(this);
}

void TreeClock::ReleaseStore(TreeClock** dstp) const {
  TreeClock* dst = AllocClock(dstp);
  // Printf("%u: ReleaseStore %u @ %p - %p\n", static_cast<u8>(root_sid_), dst->root_sid_, this, dst);
  // Printf("this:       [%u] %u (%u) %u (%u) %u (%u)\n", root_sid_
  //                                   , clk_[1], aclk_[1]
  //                                   , clk_[2], aclk_[2]
  //                                   , clk_[3], aclk_[3]);
  // Printf("dst:        [%u] %u (%u) %u (%u) %u (%u)\n", dst->root_sid_
  //                                   , dst->clk_[1], dst->aclk_[1]
  //                                   , dst->clk_[2], dst->aclk_[1]
  //                                   , dst->clk_[3], dst->aclk_[1]);
  *dst = *this;
}

TreeClock& TreeClock::operator=(const TreeClock& other) {
  // Printf("%u: Copy %u @ %p - %p\n", root_sid_, other.root_sid_, this, &other);
#if TSAN_COLLECT_STATS
  atomic_fetch_add(&ctx->num_copies, 1, memory_order_relaxed);
  if (root_sid_ != kFreeSid)
    atomic_fetch_add(&ctx->num_monocopies, 1, memory_order_relaxed);
#endif

#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++)
    clk_[i] = other.clk_[i];
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
  m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(other.clk_);
  m128* __restrict vadst = reinterpret_cast<m128*>(aclk_);
  m128 const* __restrict vasrc = reinterpret_cast<m128 const*>(other.aclk_);
  for (uptr i = 0; i < kTreeClockSize; i++) {
    m128 s1 = _mm_load_si128(&vsrc[i]);
    m128 s2 = _mm_load_si128(&vasrc[i]);
    _mm_store_si128(&vdst[i], s1);
    _mm_store_si128(&vadst[i], s2);
  }

  m128* __restrict vndst = reinterpret_cast<m128*>(nodes_);
  m128 const* __restrict vnsrc = reinterpret_cast<m128 const*>(other.nodes_);
  for (uptr i = 0; i < kTreeClockSize2; i++) {
    m128 s = _mm_load_si128(&vnsrc[i]);
    _mm_store_si128(&vndst[i], s);
  }

  root_sid_ = other.root_sid_;

  // Sid src_root_sid = other.root_sid_;            // the root sid of the src clock
  // if (!IsNodeNull(src_root_sid) && src_root_sid != root_sid_)
  //   DetachNode(src_root_sid);

  // Epoch clk = Get(src_root_sid);

  // Set(src_root_sid, other.Get(src_root_sid));
  // SetAclk(src_root_sid, other.GetAclk(src_root_sid));

  // Node node = GetNode(src_root_sid);
  // node.parent = kFreeSid;
  // node.prev = kFreeSid;
  // node.next = kFreeSid;

  // GetUpdatedNodesJoin(other, src_root_sid, )

#endif
  return *this;
}

void TreeClock::ReleaseStoreAcquire(TreeClock** dstp) {
  // Printf("Num relstoreacq\n");
  TreeClock* dst = AllocClock(dstp);
  // Printf("%u: ReleaseStoreAcquire %u\n", root_sid_, dst->root_sid_);
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    Epoch tmp = dst->clk_[i];
    dst->clk_[i] = clk_[i];
    clk_[i] = max(clk_[i], tmp);
  }
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(dst->clk_);
  m128* __restrict vclk = reinterpret_cast<m128*>(clk_);
  for (uptr i = 0; i < kTreeClockSize; i++) {
    m128 t = _mm_load_si128(&vdst[i]);
    m128 c = _mm_load_si128(&vclk[i]);
    m128 m = _mm_max_epu16(c, t);
    _mm_store_si128(&vdst[i], c);
    _mm_store_si128(&vclk[i], m);
  }
#endif
}

void TreeClock::ReleaseAcquire(TreeClock** dstp) {
#if TSAN_COLLECT_STATS
  u32 num = atomic_fetch_add(&ctx->num_rel_acq, 1, memory_order_relaxed);
  Printf("Num relacq: %u\n", num);
#endif

  TreeClock* dst = AllocClock(dstp);
  // Printf("%u: ReleaseAcquire %u\n", root_sid_, dst->root_sid_);
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    dst->clk_[i] = max(dst->clk_[i], clk_[i]);
    clk_[i] = dst->clk_[i];
  }
#else
  m128* __restrict vdst = reinterpret_cast<m128*>(dst->clk_);
  m128* __restrict vclk = reinterpret_cast<m128*>(clk_);
  for (uptr i = 0; i < kTreeClockSize; i++) {
    m128 c = _mm_load_si128(&vclk[i]);
    m128 d = _mm_load_si128(&vdst[i]);
    m128 m = _mm_max_epu16(c, d);
    _mm_store_si128(&vdst[i], m);
    _mm_store_si128(&vclk[i], m);
  }
#endif
}

}  // namespace __tsan
