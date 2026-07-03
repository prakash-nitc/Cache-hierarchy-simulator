#pragma once
// CacheConfig — the knobs that define one cache's geometry.
//
// Phase 1 models a single direct-mapped, read-only cache, so only the geometry
// fields exist here. Write/replacement policies, hit times, etc. are added in
// the phases that introduce them (SPEC sections 6.2, 8).

#include <cstdint>
#include <string>

// Which line to evict when a set is full (SPEC section 5.3).
enum class ReplacementType { LRU, FIFO, Random };

struct CacheConfig {
    std::string     name          = "L1";  // label used in reports
    uint64_t        sizeBytes     = 0;     // total data capacity in bytes
    uint64_t        blockSize     = 0;     // bytes per block/line (power of two)
    uint64_t        associativity = 1;     // ways per set; 1 == direct-mapped
    ReplacementType replacement   = ReplacementType::LRU;
    uint64_t        addrWidth     = 64;    // address width in bits (sets tagBits)
};
