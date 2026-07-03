#pragma once
// Cache — a single read-only cache of any associativity (Phase 2).
//
// Responsibilities so far:
//   * derive geometry (numSets, offset/index/tag bit widths) from the config;
//   * decompose a byte address into (offset, set index, tag);
//   * answer hit/miss for each access and tally Stats;
//   * on a full set, evict the way chosen by the ReplacementPolicy (Strategy).
//
// Writes (dirty bits, write policies) and a next level down are introduced in
// Phases 3-4 (SPEC sections 7.3, 6.3).

#include "config.h"
#include "replacement.h"
#include "stats.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <ostream>

struct CacheLine {     // one block frame (one "way")
    bool     valid = false;
    uint64_t tag   = 0;
};

struct CacheSet {
    std::vector<CacheLine> lines;   // size == associativity (1 in Phase 1)
};

class Cache {
public:
    // The three address fields plus the block address, for reporting/verbose.
    struct Decoded {
        uint64_t offset;
        uint64_t blockAddr;
        uint64_t setIndex;
        uint64_t tag;
    };

    explicit Cache(const CacheConfig& cfg);

    // Look up `addr`; returns true on a hit. `isWrite` is recorded in Stats but
    // not yet specially handled — Phase 1 treats every access as a lookup.
    bool access(uint64_t addr, bool isWrite);

    // Split an address into offset / set index / tag using this cache's geometry.
    Decoded decode(uint64_t addr) const;

    // Print geometry + stats.
    void report(std::ostream& os) const;

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
    Stats                              stats_;
};
