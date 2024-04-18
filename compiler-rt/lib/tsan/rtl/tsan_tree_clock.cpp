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

#if TSAN_COLLECT_CLOCK_STATS
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

  m128 ff = _mm_set1_epi8(0xff);
  m128* vnode = reinterpret_cast<m128*>(nodes_);
  for (uptr i = 0; i < kTreeClockSize2; i++) _mm_store_si128(&vnode[i], ff);

  // for (uptr i = 0; i < kThreadSlotCount; ++i) if (nodes_[i].parent != kFreeSid) Abort();

  root_sid_ = kFreeSid;
  stack_pos_ = -1;

  Printf("Reset clock: %p\n", this);
  magic = 0x1337;
#endif
}

ALWAYS_INLINE bool TreeClock::IsNodeNull(Sid sid) const {
  return GetRawNode(sid) == 0xffffffff;
}

ALWAYS_INLINE void TreeClock::PushChild(Sid parent, Sid child, const TreeClock* src) {
  // Printf("%u: PushChild %u -> %u\n", root_sid_, parent, child);
  // DCHECK_NE(parent, child);
  // if (parent == kFreeSid) {
  //   Printf("%u: HURRR!!! %u -> %u\n", root_sid_, parent, child);
  //   Abort();
  // }

  // if (GetNode(root_sid_).parent != kFreeSid) {
  // if (child == root_sid_) {
  //   Printf("%u: WOT!!! %u -> %u / %u -> %u -> %u\n", root_sid_,  GetNode(root_sid_).parent, root_sid_, GetNode(parent).parent, parent, child);
  //   Abort();
  // }

  // check if there is cycle
  // Sid cur_sid = parent;
  // while (cur_sid != kFreeSid) {
  //   if (child == cur_sid) {
  //     Printf("%u: !!! Cycle %u -> %u   \n", root_sid_, parent, child);
  //     PrintTreeClock();
  //     src->PrintTreeClock();

  //     // Print the cycle
  //     Sid print_cur_sid = parent;
  //     while (print_cur_sid != kFreeSid) {
  //       Printf("<- %u ", print_cur_sid);
  //       print_cur_sid = GetNode(print_cur_sid).parent;
  //     }
  //     Printf("\n");

  //     Abort();
  //   }
  //   cur_sid = GetNode(cur_sid).parent;
  // }

  Node& parent_node = GetNode(parent);
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
  parent_node.first_child = child;
  // Printf("           %u -> %u\n", parent, GetNode(parent).first_child);

  Sid cur_sid = child;
  while (cur_sid != kFreeSid) {
    cur_sid = GetNode(cur_sid).next;
    if (child == cur_sid) {
      Printf("%u: !!! PushChild cycle detected\n", root_sid_);

      // Print the cycle
      Sid print_cur_sid = child;
      do {
        Printf("%u -> ", print_cur_sid);
        print_cur_sid = GetNode(print_cur_sid).next;
      } while (child != print_cur_sid);
      Printf("\n");

      Abort();
    }
  }
}

void TreeClock::PrintTreeClock() const {
  Printf("Tree Clock %u @ %p\n", root_sid_, this);
  Printf("  stack_pos_: %u, parent: %u, prev: %u, next: %u\n", stack_pos_, GetNode(root_sid_).parent, GetNode(root_sid_).prev, GetNode(root_sid_).next);
  Printf("  has_reset: %u\n", magic);

  PrintTreeClock_(root_sid_);
}

void TreeClock::PrintTreeClock_(Sid sid, int level) const {
  for (int i = 0; i < level; ++i) Printf("-");
  Printf("[%u] (%u : %u)\n", sid, Get(sid), GetAclk(sid));

  Sid cur_sid = GetNode(sid).first_child;
  while (cur_sid != kFreeSid) {
    PrintTreeClock_(cur_sid, level+1);
    cur_sid = GetNode(cur_sid).next;
  }
}

ALWAYS_INLINE void TreeClock::GetUpdatedNodesJoin(const TreeClock* src, Sid parent, Epoch parent_clk) {
  // Printf("%u: GetUpdatedNodesJoin %u\n", root_sid_, parent);
  Node parent_node = src->GetNode(parent);
  Sid cur_node_sid = parent_node.first_child;

  while (cur_node_sid != kFreeSid) {
    Epoch src_clk = src->Get(cur_node_sid);
    Epoch this_clk = Get(cur_node_sid);
    if (this_clk < src_clk) {
      stack_[++stack_pos_] = cur_node_sid;

      // it's ok allow it. it needs to be updated as well
      if (cur_node_sid == root_sid_) {
        Printf("%u <- %u: Hmm!!!\n", root_sid_, src->root_sid_);
        PrintTreeClock();
        src->PrintTreeClock();
        Abort();
      }
      if (stack_pos_ >= 256) {
        Printf("wat!!!!\n");
        Abort();
      }
    }
    else if (src->GetAclk(cur_node_sid) <= parent_clk) break;

    cur_node_sid = src->GetNode(cur_node_sid).next;
    if (cur_node_sid == parent_node.first_child) {
      Printf("%u: GetUpdatedNodesJoin cycle detected\n", src->root_sid_);

      Sid print_sid = parent_node.first_child;
      do {
        Printf("%u -> ", print_sid);
        print_sid = src->GetNode(print_sid).next;
      } while (print_sid != parent_node.first_child);
      Printf("\n");

      Abort();
    }
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
  if (!IsNodeNull(src_root_sid) && src_root_sid != root_sid_) DetachNode(src_root_sid);
  // if (GetNode(root_sid_).parent != kFreeSid) {
  //   Printf("%u: WOT1!!! root has %u -> %u.\n", root_sid_, GetNode(root_sid_).parent, root_sid_);
  //   Abort();
  // }

  if (GetNode(root_sid_).parent != kFreeSid) {
    Printf("%u: 0a Parent of root is not kFreeSid!\n", root_sid_);
    PrintTreeClock();
    Abort();
  }

  if (static_cast<u16>(GetAclk(root_sid_)) > 0) {
    Printf("%u: 0 Aclk of root must be 0!\n", root_sid_);
    PrintTreeClock();
    Abort();
  }

  // clock assignments
  Set(src_root_sid, src_src_root_clk);               // update epoch
  SetAclk(src_root_sid, Get(root_sid_));        // to indicate that the root sid was updated at this epoch
  if (static_cast<u16>(GetAclk(root_sid_)) > 0) {
    Printf("%u: 1 Aclk of root must be 0!\n", root_sid_);
    PrintTreeClock();
    Abort();
  }

  // only push child if it is not the root node
  if (root_sid_ != src_root_sid) PushChild(root_sid_, src_root_sid, src);
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

    if (cur_node_sid == root_sid_) {
      Printf("%u: root should not be acquiring\n", root_sid_);
    }
    PushChild(src->GetNode(cur_node_sid).parent, cur_node_sid, src);
    // if (GetNode(root_sid_).parent != kFreeSid) {
    //   Printf("%u: WOT6!!! root has %u -> %u.\n", root_sid_, GetNode(root_sid_).parent, root_sid_);
    //     PrintTreeClock();
    //     src->PrintTreeClock();
    //   Abort();
    // }
    GetUpdatedNodesJoin(src, cur_node_sid, cur_node_clock);
  }
  // Printf("\n");

  // Printf("res:       [%u] %u (%u) %u (%u) %u (%u)\n", root_sid_
  //                                   , clk_[1], aclk_[1]
  //                                   , clk_[2], aclk_[2]
  //                                   , clk_[3], aclk_[3]);
  // if (GetNode(root_sid_).parent != kFreeSid) {
  //   Printf("%u: WOT2!!! root has %u -> %u.\n", root_sid_, GetNode(root_sid_).parent, root_sid_);
  //   PrintTreeClock();
  //   src->PrintTreeClock();
  //   Abort();
  // }
  if (static_cast<u16>(GetAclk(root_sid_)) > 0) {
    Printf("%u: 2 Aclk of root must be 0!\n", root_sid_);
    PrintTreeClock();
    Abort();
  }

  if (GetNode(root_sid_).parent != kFreeSid) {
    Printf("%u: 0b Parent of root is not kFreeSid!\n", root_sid_);
    PrintTreeClock();
    Abort();
  }

#endif
}

static TreeClock* AllocClock(TreeClock** dstp) {
  if (UNLIKELY(!*dstp)) {
    *dstp = New<TreeClock>();
    Printf("Alloc clock @ %p\n", *dstp);
  }
  return *dstp;
}

void TreeClock::Release(TreeClock** dstp) const {
  TreeClock* dst = AllocClock(dstp);
  // Printf("%u: Release %u @ %p\n", static_cast<u8>(root_sid_), dst->root_sid_, this);
  // dst->Acquire(this);
  // Is dst <= this? (Does it satisfy Lemma 2: Monotonicity of copies?)
  // Why did they do acquire instead of copy?
  *dst = *this;
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

ALWAYS_INLINE void TreeClock::GetUpdatedNodesCopy(const TreeClock& other, Sid parent, Epoch parent_clk) {
  // Printf("%u: GetUpdatedNodesCopy %u\n", root_sid_, parent);
  Node parent_node = other.GetNode(parent);
  Sid cur_node_sid = parent_node.first_child;

  while (cur_node_sid != kFreeSid) {
    Epoch src_clk = other.Get(cur_node_sid);
    Epoch this_clk = Get(cur_node_sid);
    if (this_clk < src_clk) stack_[++stack_pos_] = cur_node_sid;
    else if (cur_node_sid == root_sid_) stack_[++stack_pos_] = cur_node_sid;
    else if (other.GetAclk(cur_node_sid) <= parent_clk) break;

    cur_node_sid = other.GetNode(cur_node_sid).next;
    if (cur_node_sid == parent_node.first_child) {
      Printf("%u: GetUpdatedNodesCopy cycle detected\n", other.root_sid_);
      Sid print_sid = parent_node.first_child;
      do {
        Printf("%u -> ", print_sid);
        print_sid = other.GetNode(print_sid).next;
      } while (print_sid != parent_node.first_child);
      Printf("\n");
      Abort();
    }
  }
}

TreeClock& TreeClock::operator=(const TreeClock& other) {
  // Printf("%u: Copy %u @ %p - %p\n", root_sid_, other.root_sid_, this, &other);
#if TSAN_COLLECT_CLOCK_STATS
  atomic_fetch_add(&ctx->num_copies, 1, memory_order_relaxed);
  if (root_sid_ != kFreeSid)
    atomic_fetch_add(&ctx->num_monocopies, 1, memory_order_relaxed);
#endif

#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++)
    clk_[i] = other.clk_[i];
#else
  // m128* __restrict vdst = reinterpret_cast<m128*>(clk_);
  // m128 const* __restrict vsrc = reinterpret_cast<m128 const*>(other.clk_);
  // m128* __restrict vadst = reinterpret_cast<m128*>(aclk_);
  // m128 const* __restrict vasrc = reinterpret_cast<m128 const*>(other.aclk_);
  // for (uptr i = 0; i < kTreeClockSize; i++) {
  //   m128 s1 = _mm_load_si128(&vsrc[i]);
  //   m128 s2 = _mm_load_si128(&vasrc[i]);
  //   _mm_store_si128(&vdst[i], s1);
  //   _mm_store_si128(&vadst[i], s2);
  // }

  // m128* __restrict vndst = reinterpret_cast<m128*>(nodes_);
  // m128 const* __restrict vnsrc = reinterpret_cast<m128 const*>(other.nodes_);
  // for (uptr i = 0; i < kTreeClockSize2; i++) {
  //   m128 s = _mm_load_si128(&vnsrc[i]);
  //   _mm_store_si128(&vndst[i], s);
  // }

  // root_sid_ = other.root_sid_;

  Sid other_root_sid = other.root_sid_;            // the root sid of the src clock
  if (!IsNodeNull(other_root_sid) && other_root_sid != root_sid_)
    DetachNode(other_root_sid);

  Epoch this_other_root_clk = Get(other_root_sid);

  Set(other_root_sid, other.Get(other_root_sid));
  SetAclk(other_root_sid, other.GetAclk(other_root_sid));

  if (GetNode(root_sid_).parent != kFreeSid) {
    Printf("%u: 1a Parent of root is not kFreeSid!\n", root_sid_);
    PrintTreeClock();
    Abort();
  }

  if (static_cast<u16>(GetAclk(root_sid_)) > 0) {
    Printf("%u: 3a Aclk of root must be 0!\n", root_sid_);
    PrintTreeClock();
    Abort();
  }
  if (static_cast<u16>(other.GetAclk(other_root_sid)) > 0) {
    Printf("%u = %u: 3b Aclk of root must be 0!\n", root_sid_, other.root_sid_);
    other.PrintTreeClock();
    Abort();
  }

  Node& node = GetNode(other_root_sid);
  node.parent = kFreeSid;
  node.prev = kFreeSid;
  node.next = kFreeSid;

  GetUpdatedNodesCopy(other, other_root_sid, this_other_root_clk);

  bool pushed_root = root_sid_ == kFreeSid || root_sid_ == other_root_sid;

  while (stack_pos_ >= 0) {
    Sid cur_node_sid = stack_[stack_pos_--];
    // Printf("%u ", cur_node_sid);
    Epoch cur_node_clock = Get(cur_node_sid);

    // need to reorder this node in the tree
    // TODO: CHECK THE SECOND PREDICATE
    if (!IsNodeNull(cur_node_sid)) DetachNode(cur_node_sid);

    Set(cur_node_sid, other.Get(cur_node_sid));
    SetAclk(cur_node_sid, other.GetAclk(cur_node_sid));

    // if (cur_node_sid == root_sid_) pushed_root = true;
    PushChild(other.GetNode(cur_node_sid).parent, cur_node_sid, &other);
    GetUpdatedNodesCopy(other, cur_node_sid, cur_node_clock);
  }

  // TODO: DID THEY PROVE THIS?
  // root is changed, nodes that were not in the previous tree will be completely lost
  // if (!pushed_root) {
    // Printf("%u = %u: GG!!!\n", root_sid_, other_root_sid);
    // PrintTreeClock();
    // other.PrintTreeClock();
    // Abort();
    // PushChild(other_root_sid, root_sid_, &other);
    // SetAclk(root_sid_, Get(other_root_sid));
  // }

  root_sid_ = other_root_sid;
  if (static_cast<u16>(GetAclk(root_sid_)) > 0) {
    Printf("%u: 4 Aclk of root must be 0!\n", root_sid_);
    PrintTreeClock();
    Abort();
  }
  // if (GetNode(root_sid_).parent != kFreeSid) {
  //   Printf("%u: WOT3!!! root has %u -> %u.\n", root_sid_, GetNode(root_sid_).parent, root_sid_);
  //   Abort();
  // }

  if (GetNode(root_sid_).parent != kFreeSid) {
    Printf("%u: 1b Parent of root is not kFreeSid!\n", root_sid_);
    PrintTreeClock();
    Abort();
  }

#endif
  return *this;
}

void TreeClock::ReleaseStoreAcquire(TreeClock** dstp) {
  Abort();
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
#if TSAN_COLLECT_CLOCK_STATS
  u32 num = atomic_fetch_add(&ctx->num_rel_acq, 1, memory_order_relaxed);
  // Printf("Num relacq: %u\n", num);
#endif
  TreeClock* dst = AllocClock(dstp);
  // Printf("%u: ReleaseAcquire %u\n", root_sid_, dst->root_sid_);
#if !TSAN_VECTORIZE
  for (uptr i = 0; i < kThreadSlotCount; i++) {
    dst->clk_[i] = max(dst->clk_[i], clk_[i]);
    clk_[i] = dst->clk_[i];
  }
#else
  // m128* __restrict vdst = reinterpret_cast<m128*>(dst->clk_);
  // m128* __restrict vclk = reinterpret_cast<m128*>(clk_);
  // for (uptr i = 0; i < kTreeClockSize; i++) {
  //   m128 c = _mm_load_si128(&vclk[i]);
  //   m128 d = _mm_load_si128(&vdst[i]);
  //   m128 m = _mm_max_epu16(c, d);
  //   _mm_store_si128(&vdst[i], m);
  //   _mm_store_si128(&vclk[i], m);
  // }

  // Printf("%u - %u: ReleaseAcquire begin\n", root_sid_, dst->root_sid_);

  if (dst == this) {
    Printf("!!! Wow dst == this\n");
    Abort();
  }

  Acquire(dst);
  // if (GetNode(root_sid_).parent != kFreeSid) {
  //   Printf("%u: WOT4!!! root has %u -> %u.\n", root_sid_, GetNode(root_sid_).parent, root_sid_);
  //   PrintTreeClock();
  //   dst->PrintTreeClock();
  //   Abort();
  // }
  *dst = *this;
  // if (dst->GetNode(root_sid_).parent != kFreeSid) {
  //   Printf("%u: WOT5!!! root has %u -> %u.\n", root_sid_, dst->GetNode(root_sid_).parent, root_sid_);
  //   Abort();
  // }

  // Printf("%u - %u: ReleaseAcquire end\n", root_sid_, dst->root_sid_);
#endif
}

}  // namespace __tsan
