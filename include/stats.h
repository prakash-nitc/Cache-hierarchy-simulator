#pragma once
// Stats — per-cache access counters.
//
// Phase 1 needs only the access/hit/miss tallies (plus read/write split, which
// is free to carry). Write-backs and the 3-C breakdown are added in their phases
// (SPEC sections 6.2, 5.5).

#include <cstdint>

struct Stats {
    uint64_t accesses   = 0;
    uint64_t hits       = 0;
    uint64_t misses     = 0;
    uint64_t reads      = 0;
    uint64_t writes     = 0;
    uint64_t writebacks = 0;   // dirty evictions written to the level below
    uint64_t compulsory = 0;   // 3-C split of misses (only when classify3C is on):
    uint64_t capacity   = 0;   //   first-ever touch / would also miss fully-assoc /
    uint64_t conflict   = 0;   //   would have hit fully-assoc (blame associativity)

    double missRate() const { return accesses ? double(misses) / double(accesses) : 0.0; }
    double hitRate()  const { return accesses ? double(hits)   / double(accesses) : 0.0; }
};
