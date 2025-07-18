//===-- tsan_rex.cpp --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
// Implementation of the ReX redundancy filter.
//===----------------------------------------------------------------------===//

#include "tsan_filter.h"
#include "tsan_rtl.h"
#include "tsan_flags.h"
#include "sanitizer_common/sanitizer_placement_new.h"

namespace __tsan {

//=============================== Statistics =================================//

// Total number of memory accesses processed by the filter.
static atomic_uint64_t stats_total_accesses;

// Can add more detailed counters for debugging
static atomic_uint64_t stats_filtered_intra_thread;
static atomic_uint64_t stats_filtered_inter_thread;

void PrintFilterStats() {
  if (!flags()->enable_filter)
    return;

  u64 total = atomic_load(&stats_total_accesses, memory_order_relaxed);
  u64 intra = atomic_load(&stats_filtered_intra_thread, memory_order_relaxed);
  u64 inter = atomic_load(&stats_filtered_inter_thread, memory_order_relaxed);
  u64 filtered = intra + inter;
  u64 not_filtered = total - filtered;

  Printf("ThreadSanitizer: ReX Filter Stats\n");
  Printf("  Total memory accesses processed: %llu\n", total);
  Printf("  Accesses filtered (redundant): %llu (%d %%)\n",
         filtered, static_cast<int>(filtered * 100.0 / total));
  Printf("    - Intra-thread redundancy:   %llu (%d %% of filtered)\n",
         intra, static_cast<int>(intra * 100.0 / filtered));
  Printf("    - Inter-thread redundancy:   %llu (%d %% of filtered)\n",
         inter, static_cast<int>(inter * 100.0 / filtered));
  Printf("  Accesses not filtered (passed to TSan): %llu (%d %%)\n",
         not_filtered, static_cast<int>(not_filtered * 100.0 / total));
}

//=========================== Trie Implementation ============================//

TrieNode::TrieNode()
    : children_(nullptr) {
  atomic_store(&write_tids_, 0, memory_order_relaxed);
  atomic_store(&read_tids_, 0, memory_order_relaxed);
}

TrieNode::~TrieNode() {
  if (children_) {
    // Iterate through all children and delete them recursively.
    // The lambda must return bool to conform to the forEach API.
    children_->forEach([&](auto& pair) -> bool {
      DestroyAndFree(pair.second);
      return true;
    });
    // Finally, delete the map container itself.
    DestroyAndFree(children_);
  }
}

bool TrieNode::CheckAndAdd(u32 tid, bool is_write) {
  // Select the appropriate atomic "stack" based on the access type.
  atomic_uint64_t* target_tids = is_write ? &write_tids_ : &read_tids_;

  u64 current_tids = atomic_load(target_tids, memory_order_relaxed);
  while (true) {
    const u32 tid1 = current_tids & 0xFFFFFFFF;
    const u32 tid2 = current_tids >> 32;

    // Intra-thread redundancy: this thread has been here before with this context.
    if (tid1 == tid || tid2 == tid) {
      VPrintf(1, "\tThread %d: Intra-thread redundancy: is_write=%d\n\n", tid,
              is_write);
      atomic_fetch_add(&stats_filtered_intra_thread, 1, memory_order_relaxed);
      return true;
    }

    // Inter-thread redundancy: two other threads have already been here.
    if (tid1 != 0 && tid2 != 0) {
      VPrintf(1,
              "\tThread %d: Inter-thread redundancy: tid1=%u tid2=%u "
              "is_write=%d\n\n",
              tid, tid1, tid2, is_write);
      atomic_fetch_add(&stats_filtered_inter_thread, 1, memory_order_relaxed);
      return true;
    }

    VPrintf(1, "\tThread %d: NO REDUNDANCY: tid1=%u tid2=%u is_write=%d\n\n",
            tid, tid1, tid2, is_write);

    // Not redundant. Try to add the new tid to an empty slot.
    u64 new_tids;
    if (tid1 == 0) {
      new_tids = current_tids | tid;
    } else { // tid2 must be 0
      new_tids = current_tids | ((u64)tid << 32);
    }

    // Attempt to atomically update the tids.
    // If it succeeds, we "won the race", and the event is not redundant.
    if (atomic_compare_exchange_weak(target_tids, &current_tids, new_tids,
                                     memory_order_acq_rel))
      return false;
    // If CAS failed, another thread modified tids.
    // The loop continues with the new value of 'current_tids'.
  }
}

TrieNode* TrieNode::GetOrCreateChild(uptr event_id) {
  // A single lock guards the entire operation to ensure thread safety
  // for both creating the map and adding elements to it.
  Lock lock(&children_mtx_);

  // Lazily create the children map on first use.
  if (children_ == nullptr) {
    children_ = New<DenseMap<uptr, TrieNode*>>();
    children_->init(16); // Initialize with a reasonable capacity.
  }

  // Look for an existing child node.
  auto* bucket = children_->find(event_id);
  if (bucket) {
    return bucket->second; // Return existing node.
  }

  // If not found, create a new one.
  TrieNode* new_node = New<TrieNode>();
  (*children_)[event_id] = new_node;
  return new_node;
}

//======================= FilterHistory Implementation =======================//

FilterHistory::FilterHistory() {}

FilterHistory::~FilterHistory() {
  Printf("FilterHistory::~FilterHistory() called\n");
  // The global g_rex_filter is never destroyed in TSan's lifecycle,
  // but for completeness, a proper destructor would look like this.
  Lock lock(&pc_map_mtx_);
  pc_map_.forEach([&](auto& pc_pair) -> bool {
    // pc_pair.second is the addr_map
    pc_pair.second->forEach([&](auto& addr_pair) -> bool {
      // addr_pair.second is the TrieNode root
      DestroyAndFree(addr_pair.second);
      return true;
    });
    DestroyAndFree(pc_pair.second);
    return true;
  });
}

bool FilterHistory::CheckRedundancy(uptr pc, uptr addr, bool is_write,
                                    const LocksetContext& context, u32 tid) {
  atomic_fetch_add(&stats_total_accesses, 1, memory_order_relaxed);

  // Because tid = 0 means "empty slot" in the trie
  tid++;

  VPrintf(1, "Thread %d: CheckRedundancy: pc=%p addr=%p is_write=%d\n",
          tid, (void*)pc, (void*)addr, is_write);
  // Level 1: Find or create the address-to-Trie map for the given PC.
  DenseMap<uptr, TrieNode*>* addr_map;
  {
    Lock lock(&pc_map_mtx_);
    VPrintf(1, "Level 1: Looking up addr_map for PC=%p\n", (void*)pc);
    auto* map_bucket = pc_map_.find(pc);
    if (map_bucket) {
      addr_map = map_bucket->second;
      VPrintf(1, "Found existing addr_map\n");
    } else {
      addr_map = New<DenseMap<uptr, TrieNode*>>();
      addr_map->init(16);  // Sensible default size
      pc_map_[pc] = addr_map;
      VPrintf(1, "Created new addr_map\n");
    }
  }

  // Level 2: Find or create the Trie root for the given memory address.
  // This map needs its own synchronization. For simplicity, we can reuse
  // the global mutex, but a dedicated mutex per addr_map would be better
  // for performance (a future optimization).
  TrieNode* root;
  {
    Lock lock(&pc_map_mtx_);  // Reusing the global lock for simplicity.
    VPrintf(1, "Level 2: Looking up root node for addr=%p\n", (void*)addr);
    auto* root_bucket = addr_map->find(addr);
    if (root_bucket) {
      root = root_bucket->second;
      VPrintf(1, "Found existing root node\n");
    } else {
      root = New<TrieNode>();
      (*addr_map)[addr] = root;
      VPrintf(1, "Created new root node\n");
    }
  }

  // Level 3: Traverse the Trie using the concurrency context.
  VPrintf(1, "Level 3: Traversing trie with context size=%ud\n",
          context.context_internal.size());
  TrieNode* current_node = root;
  context.context_internal.forEach([&](const auto& bucket) -> bool {
    VPrintf(1, "  Context[i]=%p\n", (void*)bucket.first);
    current_node = current_node->GetOrCreateChild(bucket.first);
    return true;
  });

  // Finally, check for redundancy at the leaf node.
  return current_node->CheckAndAdd(tid, is_write);
}


  //=== Global Initialization ===//

  FilterHistory* g_filter = nullptr;

  void InitializeFilter() {
  // Check the flag provided by the user.
  if (!flags()->enable_filter)
    return;

  // Create the global history object if it doesn't exist.
  // This is safe because Initialize is called before any threads are created.
  if (g_filter == nullptr) {
    // In a real TSan integration, this would use a custom allocator.
    // For now, standard 'new' is fine.
    g_filter = New<FilterHistory>();
    VPrintf(1, "ThreadSanitizer: ReX redundancy filter initialized.\n");
  }
}

}  // namespace __tsan