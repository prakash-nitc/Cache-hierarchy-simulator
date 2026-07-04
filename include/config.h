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

// What a write HIT does (SPEC section 5.4): write-back marks the line dirty
// and defers the lower-level write until eviction; write-through forwards
// every store immediately.
enum class WritePolicy { WriteBack, WriteThrough };

// What a write MISS does: write-allocate fetches the block then writes it;
// no-write-allocate ("write-around") sends the store below and caches nothing.
enum class AllocPolicy { WriteAllocate, NoWriteAllocate };

struct CacheConfig {
    std::string     name          = "L1";  // label used in reports
    uint64_t        sizeBytes     = 0;     // total data capacity in bytes
    uint64_t        blockSize     = 0;     // bytes per block/line (power of two)
    uint64_t        associativity = 1;     // ways per set; 1 == direct-mapped
    ReplacementType replacement   = ReplacementType::LRU;
    WritePolicy     writePolicy   = WritePolicy::WriteBack;      // common pairing
    AllocPolicy     allocPolicy   = AllocPolicy::WriteAllocate;  // with write-back
    uint64_t        addrWidth     = 64;    // address width in bits (sets tagBits)
};
