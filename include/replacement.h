#pragma once
// ReplacementPolicy — the Strategy interface for eviction decisions.
//
// The Cache calls exactly three hooks at fixed points and never knows which
// concrete policy is installed:
//   * onAccess(set, way) — a resident line was hit;
//   * onInsert(set, way) — a line was (re)filled after a miss;
//   * getVictim(set)     — the set is full: which way do we evict?
//
// Policies only ever *observe* accesses and *return a way index*; they cannot
// touch cache state (tags, valid bits), so a policy bug can cost performance
// but never correctness. Adding a new policy (PLRU, LFU, Belady...) means
// adding a class here — cache.cpp is never edited (SPEC section 7.4).

#include "config.h"   // ReplacementType

#include <cstddef>
#include <memory>

class ReplacementPolicy {
public:
    virtual ~ReplacementPolicy() = default;
    virtual void   onAccess(size_t set, size_t way) = 0;  // a frame was hit
    virtual void   onInsert(size_t set, size_t way) = 0;  // a frame was filled
    virtual size_t getVictim(size_t set) = 0;             // pick a way to evict
};

// Human-readable policy name for reports ("LRU", "FIFO", "Random").
const char* replName(ReplacementType t);

std::unique_ptr<ReplacementPolicy>
makeReplacement(ReplacementType t, size_t numSets, size_t assoc);
