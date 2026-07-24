#pragma once
// CacheHierarchy — owns the chain L1 -> L2 -> ... -> Memory.
//
// The hierarchy's job is deliberately small, because the recursion does the
// real work: it builds the chain (deepest level first, so every cache can
// point at the level below it), sends each CPU access to L1, expands trace
// records into CPU accesses (M = load THEN store), and derives the metrics
// that only make sense across levels — global miss rates and AMAT
// (SPEC sections 6.2, 7.5).

#include "cache.h"
#include "config.h"
#include "memory.h"
#include "trace_reader.h"

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

class CacheHierarchy {
public:
    // `cfgs` is outermost-first: cfgs[0] = L1, cfgs[1] = L2, ...
    // memTime = cycles for a main-memory access (the AMAT recursion's floor).
    CacheHierarchy(const std::vector<CacheConfig>& cfgs, double memTime);

    // One CPU access, sent to L1; the next-pointer recursion handles the
    // rest. Returns true on an L1 hit.
    bool access(uint64_t addr, bool isWrite);

    // Translate one trace record into CPU accesses: L = read, S = write,
    // M = load THEN store (two accesses), I = ignored (D-cache study).
    // If vlog is non-null, each access is echoed with its L1 decode.
    void feed(const Access& a, std::ostream* vlog = nullptr);

    // AMAT built bottom-up: the miss penalty of level i IS the AMAT of
    // everything below it. Uses each level's LOCAL miss rate.
    double computeAMAT() const;

    // Per-level reports (local + global miss rates), memory traffic, AMAT.
    void reportAll(std::ostream& os) const;

    // Machine-readable stats blob for scripts/sweep.py: config + counters +
    // rates per level, memory traffic, AMAT. One JSON object on one stream.
    void reportJson(std::ostream& os, const std::string& tracePath) const;

    // Run every level's invariant checks; true = all OK.
    bool checkInvariants(std::ostream& os) const;

    size_t        numLevels()     const { return levels_.size(); }
    const Cache&  level(size_t i) const { return *levels_[i]; }
    const Memory& memory()        const { return mem_; }

private:
    void one(uint64_t addr, bool isWrite, char opc, std::ostream* vlog);

    std::vector<std::unique_ptr<Cache>> levels_;   // [0] = L1 (outermost)
    Memory   mem_;
    double   memTime_;
    uint64_t fed_ = 0;                             // verbose-echo numbering
};
