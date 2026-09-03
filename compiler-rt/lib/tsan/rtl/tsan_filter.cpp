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

// Out of line so that DenseMap's internals -- and the placement new they need
// -- are instantiated here rather than in every includer of tsan_rtl.h.
LocksetContext::LocksetContext() = default;
LocksetContext::~LocksetContext() = default;

void LocksetContext::process_lock(uptr addr) { ++internal_ctx[addr]; }

void LocksetContext::process_unlock(uptr addr) {
  auto *bucket = internal_ctx.find(addr);
  if (bucket) {
    bucket->second--;
    if (bucket->second == 0)
      internal_ctx.erase(bucket);
  }
}

//=============================== Statistics =================================//

// Total number of memory accesses processed by the filter.
static atomic_uint64_t stats_total_accesses;

// Can add more detailed counters for debugging
static atomic_uint64_t stats_filtered_intra_thread;
static atomic_uint64_t stats_filtered_inter_thread;

void PrintFilterStats(ThreadState* thr) {
  if (!flags()->enable_filter)
    return;

  u64 total = atomic_load(&stats_total_accesses, memory_order_relaxed);
  u64 intra = atomic_load(&stats_filtered_intra_thread, memory_order_relaxed);
  u64 inter = atomic_load(&stats_filtered_inter_thread, memory_order_relaxed);
  u64 filtered = intra + inter;
  u64 not_filtered = total - filtered;

  Printf("ThreadSanitizer: ReX Filter Stats\n");
  Printf("  Total memory accesses processed: %llu\n", total);
  Printf("  Accesses filtered (redundant): %llu (%d %%)\n", filtered,
         static_cast<int>(total ? filtered * 100.0 / total : 0));
  Printf("    - Intra-thread redundancy:   %llu (%d %% of filtered)\n", intra,
         static_cast<int>(filtered ? intra * 100.0 / filtered : 0));
  Printf("    - Inter-thread redundancy:   %llu (%d %% of filtered)\n", inter,
         static_cast<int>(filtered ? inter * 100.0 / filtered : 0));
  Printf("  Accesses not filtered (passed to TSan): %llu (%d %%)\n",
         not_filtered,
         static_cast<int>(total ? not_filtered * 100.0 / total : 0));

  g_filter->PrintStats(thr);
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

const char *get_thread_status_test(ThreadContext *tctx) {
  // Check the status. A thread is "alive" if it has not yet finished.
  switch (tctx->status) {
    case ThreadStatusInvalid:
      return "invalid";
    case ThreadStatusCreated:
      return "created";
    case ThreadStatusRunning:
      return "running";
    case ThreadStatusFinished:
      return "finished";
    case ThreadStatusDead:
      return "dead";
  }
  return "unknown";
}

// Our new helper function to check if a thread ID is still valid.
static bool IsTidAlive(u32 tid) {
  // IMPORTANT: Your filter receives a tid that was incremented by 1.
  // The ThreadRegistry uses 0-indexed TIDs.
  if (tid == 0) return false; // 0 is an empty slot, not a real tid
  u32 registry_tid = tid - 1;

  // Acquire a lock while accessing the thread registry.
  ThreadRegistryLock lock(&ctx->thread_registry);

  // Find the thread context by its ID.
  ThreadContext *tctx = static_cast<ThreadContext*>(
      ctx->thread_registry.GetThreadLocked(registry_tid));

  // If there's no context (thread never existed or was fully cleaned up),
  // then it's definitely not "alive".
  if (tctx == nullptr)
    return false;

  VPrintf(1, "Thread %u status: %s\n", tid, get_thread_status_test(tctx));

  return tctx->status == ThreadStatusRunning;
}

bool TrieNode::CheckAndAdd(u32 my_tid, bool is_write) {
  // Select the appropriate atomic "stack" based on the access type.
  atomic_uint64_t* target_tids = is_write ? &write_tids_ : &read_tids_;
  u64 current_tids = atomic_load(target_tids, memory_order_relaxed);

  for (;;) {
    const u32 tid1 = current_tids & 0xFFFFFFFF;
    const u32 tid2 = current_tids >> 32;

    // 1. Check intra-thread redundancy: this thread has been here before with
    // this context.
    if (tid1 == my_tid || tid2 == my_tid) {
      VPrintf(1, "\tThread %d: INTRA-THREAD REDUNDANCY: is_write=%d\n\n",
              my_tid, is_write);
      atomic_fetch_add(&stats_filtered_intra_thread, 1, memory_order_relaxed);
      return true;  // Redundant
    }

    // 2. Lazy cleanup and liveness check.
    u32 live_tid1 = 0;
    if (tid1 != 0 && IsTidAlive(tid1)) {
      VPrintf(1, "TID1=%u is alive\n", tid1);
      live_tid1 = tid1;
    } else if (tid1 != 0) {
      VPrintf(1, "TID1=%u is dead\n", tid1);
    }

    u32 live_tid2 = 0;
    if (tid2 != 0 && IsTidAlive(tid2)) {
      VPrintf(1, "TID2=%u is alive\n", tid2);
      live_tid2 = tid2;
    } else if (tid2 != 0) {
      VPrintf(1, "TID2=%u is dead\n", tid2);
    }

    // If the new "cleaned" value is different from the old one, we found
    // garbage.
    u64 cleaned_tids = ((u64)live_tid2 << 32) | live_tid1;
    if (cleaned_tids != current_tids) {
      VPrintf(1, "Found dead TIDs: current=0x%llx cleaned=0x%llx\n",
              current_tids, cleaned_tids);
      // Try to atomically replace the value containing "dead" TIDs with the
      // cleaned one.
      if (atomic_compare_exchange_weak(target_tids, &current_tids, cleaned_tids,
                                       memory_order_acq_rel)) {
        VPrintf(1, "Successfully cleaned dead TIDs\n");
        // Successfully cleaned. Restart the loop with the new, clean value.
        current_tids = cleaned_tids;
      } else {
        VPrintf(1, "CAS failed - another thread cleaned TIDs\n");
      }

      // Failed - another thread beat us to it. Just restart the loop.
      // current_val is updated by the failed CAS.
      continue;
    }

    // --- From this point on, we know that tid1 and tid2 (if not 0) are "live"
    // ---

    // 3. Inter-thread redundancy: two other threads have already been here.
    if (tid1 != 0 && tid2 != 0) {
      VPrintf(1,
              "\tThread %d: INTER-THREAD REDUNDANCY: tid1=%u tid2=%u "
              "is_write=%d\n\n",
              my_tid, tid1, tid2, is_write);
      atomic_fetch_add(&stats_filtered_inter_thread, 1, memory_order_relaxed);
      return true; // Redundant
    }

    VPrintf(1, "\tThread %d: NO REDUNDANCY: tid1=%u tid2=%u is_write=%d\n",
            my_tid, tid1, tid2, is_write);

    // 4. Not redundant. Try to add the new tid to an empty slot.
    u64 new_tids;
    if (tid1 == 0)
      new_tids = current_tids | my_tid;
    else // tid2 must be 0
      new_tids = current_tids | ((u64)my_tid << 32);

    // Attempt to atomically update the tids.
    // If it succeeds, we "won the race", and the event is not redundant.
    if (atomic_compare_exchange_weak(target_tids, &current_tids, new_tids,
                                     memory_order_acq_rel)) {
      VPrintf(1, "Successfully added tid %u to the trie\n\n", my_tid);
      return false;
    }
    VPrintf(1, "CAS failed - another thread added tid %u to the trie\n\n",
            my_tid);
    // If CAS failed, another thread modified tids.
    // The loop continues with the new value of 'current_tids'.
  }
}

// #define TSAN_LOCKFREE_TRIE 1
#ifdef TSAN_LOCKFREE_TRIE
// Remove Mutex children_mtx_;
// Instead of DenseMap we need a concurrent alternative or a more complex manual
// implementation. For starters we can try to do this with DenseMap and atomics,
// but this requires very careful memory handling.
TrieNode* TrieNode::GetOrCreateChild(uptr event_id) {
  // 1. First try to read without locking
  if (children_ && children_->find(event_id)) {
    return (*children_)[event_id];
  }

  // 2. Node doesn't exist. Create a new one locally.
  TrieNode* new_node = New<TrieNode>();

  // 3. Now synchronization is needed for insertion
  // This is where it gets complicated. Simple CAS won't work
  // for the entire DenseMap.

  // Simpler approach than full lock-free map:
  // Keep the lock, BUT only during creation/insertion.
  // Reading can be done without locking if using proper memory barriers.

  // True lock-free implementation:
  // Would require either a ready-made lock-free hash map, or manual
  // implementation using CAS cycles for pointers to the "head" of list in each
  // hash table bucket. This is a very complex topic.

  // Implementation proposed in the paper that allows "race and leak":
  // Lock lock(&children_mtx_); // Lock still needed to protect the map itself
  if (children_ == nullptr) {
    children_ = New<DenseMap<uptr, TrieNode*>>();
    children_->init(16);
  }
  auto* bucket = children_->find(event_id);
  if (bucket) {
    DestroyAndFree(new_node);  // Don't forget to free if not needed
    return bucket->second;
  }

  // Insert new node
  (*children_)[event_id] = new_node;
  return new_node;

  // To make this lock-free, the find/insert operation in DenseMap itself
  // would need to be atomic, which it isn't. So without replacing DenseMap with
  // a concurrent alternative we can't fully get rid of locking.
}
#else
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
  if (bucket)
    return bucket->second; // Return existing node.

  // If not found, create a new one.
  TrieNode* new_node = New<TrieNode>();
  (*children_)[event_id] = new_node;
  return new_node;
}
#endif  // TSAN_LOCKFREE_TRIE

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
                                    const ThreadState* thr) {
  atomic_fetch_add(&stats_total_accesses, 1, memory_order_relaxed);

  VPrintf(1, "Thread %d: CheckRedundancy: pc=%p addr=%p is_write=%d\n",
          thr->tid, (void*)pc, (void*)addr, is_write);
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
          thr->filter_ctx.internal_ctx.size());

  // 1. Create a canonical representation
  // InternalMmapVector<uptr> sorted_locks(context.context_internal.size());
  InternalMmapVector<uptr> sorted_locks;
  thr->filter_ctx.internal_ctx.forEach([&](auto& bucket) -> bool {
    // Add lock address as many times as it was recursively acquired
    for (int i = 0; i < bucket.second; ++i)
      sorted_locks.push_back(bucket.first);
    return true;
  });
  Sort(sorted_locks.data(), sorted_locks.size());

  // 2. Use it to traverse the Trie
  TrieNode* current_node = root;
  for (uptr lock_addr : sorted_locks) {
    VPrintf(1, "  Context[i]=%p\n", (void*)lock_addr);
    current_node = current_node->GetOrCreateChild(lock_addr);
  }

  // Finally, check for redundancy at the leaf node.
  // Increment because tid = 0 means "empty slot" in the trie
  bool is_redundant = current_node->CheckAndAdd(thr->tid + 1, is_write);
  if (is_redundant) {
    Lock lock(&filtered_stats_mtx);  // Protect the entire operation
    ++filtered_stats_map[pc];
  }
  return is_redundant;
}

void FilterHistory::OnMemoryFreed(uptr addr, uptr size) {
  Lock lock(&pc_map_mtx_);  // Protect the entire operation

  pc_map_.forEach([&](auto& pc_pair) -> bool {
    DenseMap<uptr, TrieNode*>* addr_map = pc_pair.second;

    // We need to iterate and delete, which is dangerous.
    // Better to collect keys for deletion first, then delete.
    InternalMmapVector<uptr> addrs_to_remove;
    addr_map->forEach([&](auto& addr_pair) -> bool {
      if (addr_pair.first >= addr && addr_pair.first < addr + size) {
        addrs_to_remove.push_back(addr_pair.first);
      }
      return true;
    });

    for (uptr addr_to_remove : addrs_to_remove) {
      auto* bucket = addr_map->find(addr_to_remove);
      if (bucket) {
        DestroyAndFree(bucket->second);  // Recursively delete Trie
        addr_map->erase(bucket);
      }
    }
    return true;
  });
}

void FilterHistory::PrintStats(ThreadState *thr) {
  Printf("\n=================================================\n");
  Printf("TOP10 most frequently accessed program locations:\n");
  Printf("=================================================\n");
  InternalMmapVector<detail::DenseMapPair<uptr, u64>> sorted_stats;
  {
    filtered_stats_map.forEach([&](auto& pair) -> bool {
      sorted_stats.push_back({pair.first, pair.second});
      return true;
    });
  }

  // Sort by count in descending order
  Sort(sorted_stats.data(), sorted_stats.size(),
       [](const auto& a, const auto& b) { return a.second > b.second; });

  // Print top 10 or less if there are fewer entries
  for (usize i = 0; i < sorted_stats.size() && i < 10; i++) {
    Printf("-------------------------------------------------\n");
#if !SANITIZER_GO
    // The Go runtime is built without the symbolizer, so it has no
    // StackTrace::Print to link against.
    VarSizeStackTrace stack;
    ObtainCurrentStack(thr, sorted_stats[i].first, &stack);
    stack.Print();
#endif
    // Printf("  #%lu: PC=%p\t", i + 1, (void*)sorted_stats[i].first);
    Printf("Filtered %llu times\n", sorted_stats[i].second);
    Printf("-------------------------------------------------\n");
  }
  Printf("=================================================\n");
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
    Printf("ThreadSanitizer: ReX redundancy filter initialized.\n");
  }
}

}  // namespace __tsan