#pragma once
// Stats — per-cache access counters.
//
// Phase 1 needs only the access/hit/miss tallies (plus read/write split, which
// is free to carry). Write-backs and the 3-C breakdown are added in their phases
// (SPEC sections 6.2, 5.5).

#include <cstdint>

struct Stats {
    uint64_t accesses = 0;
    uint64_t hits     = 0;
    uint64_t misses   = 0;
    uint64_t reads    = 0;
    uint64_t writes   = 0;

    double missRate() const { return accesses ? double(misses) / double(accesses) : 0.0; }
    double hitRate()  const { return accesses ? double(hits)   / double(accesses) : 0.0; }
};
