//===-- tsan_rex.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of ThreadSanitizer (TSan), a race detector.
//
// ReX: A Redundancy-based approach for Optimal Performance of
//      Precise Dynamic Race Detection.
//
// This header defines the main components for the ReX implementation.
//===----------------------------------------------------------------------===//

#ifndef TSAN_FILTER_H
#define TSAN_FILTER_H

#include "tsan_defs.h"
#include "sanitizer_common/sanitizer_atomic.h"
#include "sanitizer_common/sanitizer_dense_map.h"
#include "sanitizer_common/sanitizer_mutex.h"
#include "sanitizer_common/sanitizer_vector.h"

namespace __tsan {

// Represents a node in the Concurrency History Trie (part of Θloc).
class TrieNode {
 public:
  TrieNode();
  ~TrieNode();

  // Checks for redundancy based on the current thread ID (tid).
  // If not redundant, adds the tid to its internal state.
  // Returns true if the event is redundant, false otherwise.
  bool CheckAndAdd(u32 tid, bool is_write);

  // Gets or creates a child node corresponding to a synchronization event ID.
  TrieNode* GetOrCreateChild(uptr event_id);

 private:
  // Stores up to two 32-bit thread IDs.
  // 0 means the slot is empty.
  // Thread-safe "stack" for storing up to 2 thread IDs.
  // Uses 64 bits: 0-30 for first thread ID,
  // bit 31 as "first occupied" flag, 32-62 for second thread ID, bit 63 flag.
  atomic_uint64_t read_tids_;
  atomic_uint64_t write_tids_;

  // Lazily allocated map of children nodes.
  // Children nodes. Key - event label (mutex address),
  // value - pointer to child node.
  // Using a hash table. Starting with a lock for simplicity.
  DenseMap<uptr, TrieNode*>* children_;
  Mutex children_mtx_;  // Protects access and creation of children_ map.

  TrieNode(const TrieNode&) = delete;
  void operator=(const TrieNode&) = delete;
};

// Manages the global Concurrency History (all Θloc structures).
// It holds a map from Program Counter (PC) to the root of a Trie.
class FilterHistory {
 public:
  FilterHistory();
  ~FilterHistory();

  TrieNode* GetOrCreateTrieRoot(uptr pc);

  // The main entry point for ReX.
  // Checks if a memory access is redundant.
  // pc: Program Counter of the memory access.
  // addr: Address for memory access
  // is_write: Type of memory access (is write?)
  // context: The current thread's Concurrency Context (Γt).
  // tid: The current thread's ID.
  // Returns true if redundant, false otherwise.
  bool CheckRedundancy(uptr pc, uptr addr, bool is_write,
                       const Vector<uptr>& context, u32 tid);

 private:
  // Maps PC to a Trie root
  DenseMap<uptr, DenseMap<uptr, TrieNode*>*> pc_map_;
  Mutex pc_map_mtx_;  // Protects pc_map_

  FilterHistory(const FilterHistory&) = delete;
  void operator=(const FilterHistory&) = delete;
};

// Global instance of the ReX history manager.
extern FilterHistory *g_filter;

// Initializes the ReX subsystem.
void InitializeFilter();

// Print stats
void PrintFilterStats();

}  // namespace __tsan

#endif  // TSAN_FILTER_H