#pragma once
// MemoryLevel — the one interface every storage level implements.
//
// A Cache holds a MemoryLevel* to the level below it and never knows whether
// that is another Cache or main Memory. A miss, a write-through store, or a
// dirty write-back all become next->access(...) — the recursion bottoms out at
// Memory, the terminal level that always "hits" and just tallies traffic
// (SPEC sections 6.1, 6.2).

#include <cstdint>
#include <ostream>

class MemoryLevel {
public:
    virtual ~MemoryLevel() = default;
    // Perform an access; returns true on a hit (Memory always hits).
    virtual bool access(uint64_t addr, bool isWrite) = 0;
    virtual void report(std::ostream&) const {}
};

// Terminal level: no tags, no misses — only read/write traffic counters.
// Its counts are how we *validate* write policies: write-through must produce
// one memory write per store; write-back only one per dirty eviction.
class Memory : public MemoryLevel {
    uint64_t reads_  = 0;
    uint64_t writes_ = 0;
public:
    bool access(uint64_t addr, bool isWrite) override;
    void report(std::ostream& os) const override;
    uint64_t reads()  const { return reads_; }
    uint64_t writes() const { return writes_; }
};
