#pragma once
// Cache — a write-aware cache of any associativity (Phase 3).
//
// Responsibilities so far:
//   * derive geometry (numSets, offset/index/tag bit widths) from the config;
//   * decompose a byte address into (offset, set index, tag);
//   * answer hit/miss for each access and tally Stats;
//   * on a full set, evict the way chosen by the ReplacementPolicy (Strategy);
//   * honor all four write/alloc combinations: dirty bits, write-back counting,
//     write-through forwarding, write-around — traffic flows to the MemoryLevel
//     below via the next pointer (SPEC sections 6.3, 7.3).
//
// Chaining Cache->Cache (a real L2) arrives in Phase 4.

#include "config.h"
#include "memory.h"
#include "replacement.h"
#include "stats.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <ostream>

struct CacheLine {     // one block frame (one "way")
    bool     valid = false;
    bool     dirty = false;   // modified in cache, not yet written below (write-back)
    uint64_t tag   = 0;
};

struct CacheSet {
    std::vector<CacheLine> lines;   // size == associativity (1 in Phase 1)
};

class Cache : public MemoryLevel {
public:
    // The three address fields plus the block address, for reporting/verbose.
    struct Decoded {
        uint64_t offset;
        uint64_t blockAddr;
        uint64_t setIndex;
        uint64_t tag;
    };

    // `next` is the level below (Memory now; another Cache in Phase 4) and
    // must outlive this Cache. It receives fetches, write-backs, forwarded
    // and write-around stores.
    Cache(const CacheConfig& cfg, MemoryLevel* next);

    // Perform a read (isWrite=false) or a store (isWrite=true).
    // Returns true on a hit. Full control flow: SPEC section 6.3.
    bool access(uint64_t addr, bool isWrite) override;

    // Split an address into offset / set index / tag using this cache's geometry.
    Decoded decode(uint64_t addr) const;

    // Print geometry + stats.
    void report(std::ostream& os) const override;

    // Verify structural invariants (SPEC section 15): hits+misses == accesses
    // and no line is ever dirty while invalid. Prints any violation; true = OK.
    bool checkInvariants(std::ostream& os) const;

    const Stats& stats() const { return stats_; }
    uint64_t numSets()    const { return numSets_; }
    uint64_t offsetBits() const { return offsetBits_; }
    uint64_t indexBits()  const { return indexBits_; }
    uint64_t tagBits()    const { return tagBits_; }

private:
    // Choose the frame to fill on a miss: an invalid (empty) way if one
    // exists, otherwise the policy's victim.
    size_t pickWay(const CacheSet& set, uint64_t setIndex);

    CacheConfig                        cfg_;
    uint64_t                           numSets_    = 0;
    uint64_t                           offsetBits_ = 0;
    uint64_t                           indexBits_  = 0;
    uint64_t                           tagBits_    = 0;
    std::vector<CacheSet>              sets_;
    std::unique_ptr<ReplacementPolicy> repl_;
    MemoryLevel*                       next_ = nullptr;   // not owned
    Stats                              stats_;
};
