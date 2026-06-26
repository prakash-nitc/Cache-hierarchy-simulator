# Cache Hierarchy Simulator — Complete Specification & Reference

> **This is the single source of truth for the project.** Keep it in your repo at `docs/SPEC.md`.
> It contains the full design, the annotated reference implementation, the phase-by-phase build plan,
> validation tests, and every detail needed to build and defend the project.
>
> Companion documents (separate, optional): an interview Q&A bank and a presentation script. They are
> *not* needed to build the project; this file is.

---

## Table of contents
1. [Overview & one-line pitch](#1-overview--one-line-pitch)
2. [Why this project](#2-why-this-project)
3. [Tech choices](#3-tech-choices)
4. [Background primer: why caches exist](#4-background-primer-why-caches-exist)
5. [Core concepts (the "what")](#5-core-concepts-the-what)
6. [Architecture (the "how")](#6-architecture-the-how)
7. [Reference implementation (annotated code)](#7-reference-implementation-annotated-code)
8. [Build phases with validation gates](#8-build-phases-with-validation-gates)
9. [Directory structure](#9-directory-structure)
10. [Command-line interface](#10-command-line-interface)
11. [Golden validation example](#11-golden-validation-example)
12. [Real-trace generation](#12-real-trace-generation)
13. [Comparison report (what makes this uncommon)](#13-comparison-report-what-makes-this-uncommon)
14. [Definition of done](#14-definition-of-done)
15. [Invariants (your correctness argument)](#15-invariants-your-correctness-argument)
16. [Resume bullet & headline talking points](#16-resume-bullet--headline-talking-points)

---

## 1. Overview & one-line pitch

A configurable, multi-level (L1/L2) CPU cache simulator written in C++17. It reads real memory-access traces and models:

- **Organizations:** direct-mapped, set-associative, fully-associative.
- **Replacement policies:** LRU, FIFO, Random (extensible via the Strategy pattern).
- **Write policies:** write-back / write-through × write-allocate / no-write-allocate (all four combinations).
- **Miss classification:** every miss tagged compulsory / capacity / conflict (the 3 C's).
- **Metrics:** per-level hit/miss rates (local *and* global), write-back traffic, and **AMAT** across the hierarchy.

It ships a comparison harness that sweeps configurations over real program traces and produces a report — that report yields the concrete numbers you put on a resume.

---

## 2. Why this project

- **Cache is the single most-asked computer-architecture topic.** Building your own lets you speak to it from first principles.
- **Self-contained.** No full datapath or pipeline needed; the cache is a clean, bounded system.
- **Produces real numbers.** Hit-rate and AMAT deltas become concrete resume bullets.
- **Reinforces DSA.** LRU replacement is the classic LeetCode "LRU Cache" problem, giving you a real reason to reason about hash-map + doubly-linked-list designs and O(1) vs O(ways) trade-offs.
- **Uncommon** because it runs *real* program traces (not toy input) and ships a *comparison report*, not a single run.

---

## 3. Tech choices

| Decision | Choice | Rationale (say this in interview) |
|---|---|---|
| Language | **C++17** | Manual control over data layout; fast enough for 100M-access traces; demonstrates systems fluency. (Python is a valid fallback; the architecture is identical.) |
| Build | **Makefile** (primary), CMake optional | Transparent; you can read every compile flag. |
| Paradigm | OOP + **Strategy pattern** for replacement | Adding a new policy never touches cache logic. |
| Trace format | **Valgrind/CMU `lackey`-style** | Real, free, reproducible: any Linux binary → real trace. |
| Output | Console + **CSV** + **Markdown** report | CSV feeds plots; Markdown is human-readable. |
| Testing | Hand-traced golden tests + invariants | Lets you *prove* correctness when grilled. |

> Python alternative: keep the same class structure; swap `uint64_t` for `int`, drop the Makefile, use `argparse`. Everything else applies.

---

## 4. Background primer: why caches exist

The CPU is roughly 100× faster than DRAM. A cache is a small, fast SRAM between them that holds *recently and nearby* data so most accesses are fast. It works only because programs exhibit **locality**:

- **Temporal locality** — if you touched an address, you'll likely touch it again soon (loop counters, hot variables).
- **Spatial locality** — if you touched an address, you'll likely touch nearby ones soon (array scans, struct fields). Caching whole **blocks** (e.g., 64 B) exploits this.

The cache's entire job, for each address, is: *"Do I already have this block? (hit) or must I fetch it from below? (miss)."* Everything else — sets, ways, tags, replacement, write policy — is mechanism to answer that quickly and correctly while using limited space well.

---

## 5. Core concepts (the "what")

### 5.1 Address decomposition

Every byte address splits into three fields:

```
 MSB                                            LSB
+----------------------+-----------+------------------+
|        TAG           |   INDEX   |   BLOCK OFFSET   |
+----------------------+-----------+------------------+
   tagBits                indexBits     offsetBits
```

- `offsetBits = log2(blockSize)` — which byte inside a block.
- `indexBits  = log2(numSets)`   — which set the block maps to.
- `tagBits    = addrWidth - indexBits - offsetBits` — identifies the block within a set.

Bit extraction (all sizes are powers of two, so use masks/shifts, never `%` or `/`):

```
offset    = addr & (blockSize - 1)
blockAddr = addr >> offsetBits
setIndex  = blockAddr & (numSets - 1)
tag       = blockAddr >> indexBits
```

Reconstruct a victim's byte address (needed when the next level is also a cache):

```
blockAddr = (tag << indexBits) | setIndex
addr      =  blockAddr << offsetBits
```

### 5.2 Cache geometry

```
numSets        = cacheSize / (blockSize * associativity)
totalLines     = cacheSize / blockSize

direct-mapped  : associativity = 1            → numSets = totalLines
fully-assoc.   : associativity = totalLines   → numSets = 1, indexBits = 0
set-assoc.     : 1 < associativity < totalLines
```

`numSets = size / (block × assoc)` is *the* geometry identity — internalize it; interviewers ask you to derive it.

### 5.3 Replacement policies (per set — you only ever evict within the mapped set)

- **LRU** — evict the least-recently-used line.
- **FIFO** — evict the line inserted earliest (ignores re-access).
- **Random** — evict a uniformly random line.
- *(Stretch: PLRU tree, LFU, OPT/Belady for a theoretical lower bound.)*

Because associativity is small (typically ≤ 16), an **O(ways)** policy is fine. Key engineering judgment to articulate: the famous **O(1) LeetCode LRU** (hash map + doubly linked list) is the right tool when the LRU set is *large* (a software cache of thousands of entries); inside a hardware cache set of 8 ways, scanning 8 counters is cheaper and simpler. You implemented the simple version *on purpose* and can explain the O(1) version on demand.

### 5.4 Write policies (two orthogonal axes)

- **Hit policy:** `write-back` (mark line dirty; write to next level only on eviction) **or** `write-through` (write to next level on every store).
- **Miss policy:** `write-allocate` (load block into cache on a write miss, then write) **or** `no-write-allocate` / write-around (send the store straight to the next level; cache nothing).
- Common real pairings: **write-back + write-allocate** and **write-through + no-write-allocate**. Support all four.
- Track the **dirty bit** per line and count **write-backs** (memory write traffic). The dirty bit is what makes write-back correct: an evicted *dirty* line must be written back; a *clean* line can be silently dropped.

### 5.5 Miss classification — the 3 C's

- **Compulsory (cold)** — first ever reference to this block. Would miss even in an infinite cache. *Detect:* block address never seen before (global set of seen blocks).
- **Capacity** — would also miss in a **fully-associative cache of the same total size**. *Detect:* not compulsory **and** misses in a parallel fully-associative LRU reference cache of equal size.
- **Conflict** — caused by limited associativity; would have **hit** in that fully-associative cache. *Detect:* not compulsory **and** hits in the parallel reference cache. (Standard Hill & Smith method.)

Mitigations: **compulsory** → larger blocks, prefetching; **capacity** → larger cache; **conflict** → higher associativity, victim cache, smarter (hashed/skewed) indexing.

### 5.6 Metrics & AMAT

Per level: accesses, hits, misses, hit rate, miss rate (local **and** global), reads, writes, write-backs.

**AMAT** (average memory access time), two-level:

```
AMAT = HitTime_L1 + MissRate_L1 * ( HitTime_L2 + MissRate_L2(local) * MemTime )
```

- **Local miss rate** = misses at a level / accesses *to that level*. Used in the AMAT recursion.
- **Global miss rate** = misses at a level / *total CPU* accesses = `MR_L1 × MR_L2_local`. (Know the difference cold — it's a frequent grilling point.)
- Total memory traffic (bytes) = (block fills + write-backs) × blockSize (+ write-through stores).

How to reduce each AMAT term:

```
AMAT = HitTime + MissRate * MissPenalty
        │           │            └─ multilevel caches, critical-word-first, write buffer, victim cache
        │           └─ bigger cache / higher assoc / larger blocks / prefetch / better replacement
        └─ smaller & simpler cache, way prediction, VIPT (overlap with TLB)
```

### 5.7 Trace format (CMU/Valgrind `lackey` reduced form)

One operation per line:

```
[op] [hex address],[size]
```

- `I` (column 0, no leading space) = instruction fetch.
- ` L` = load (read), ` S` = store (write), ` M` = modify = a load **then** a store to the same address (counts as two accesses).

Example:

```
 L 7f0004d8,8
 S 7f0004e0,8
 M 04ec4af0,4
I 04e4d8c4,3
```

Default behavior: route `L` and the load part of `M` as reads; `S` and the store part of `M` as writes, all to the D-cache. `I` lines are ignored by default (data-cache focus) but can optionally feed a separate instruction cache. Also accept a simple `R/W <hex>` fallback format.

---

## 6. Architecture (the "how")

### 6.1 Module map

```
                +--------------------+
   trace file → |   TraceReader      |  (op, address, size) stream
                +---------+----------+
                          │
                          ▼
                +--------------------+      drives
                |  CacheHierarchy    |───────────────► computes AMAT, aggregates stats
                +----+----------+----+
                     │          │
                     ▼          ▼
                +--------+   +--------+        recursive next-level pointer
                |  L1    |──►|  L2    |──► ... ──► +-----------+
                | Cache  |   | Cache  |            |  Memory   |  (terminal: always "hits")
                +---+----+   +---+----+            +-----------+
                    │            │
                    ▼            ▼
            +-----------------------------+
            | ReplacementPolicy (Strategy)|  LRU / FIFO / Random
            +-----------------------------+

   Config/CLI ──► builds the hierarchy.   StatsReporter ──► console / CSV / Markdown.
   scripts/sweep.py ──► runs many configs, builds the comparison report.
```

### 6.2 Class/struct definitions

```cpp
enum class WritePolicy     { WriteBack, WriteThrough };
enum class AllocPolicy     { WriteAllocate, NoWriteAllocate };
enum class ReplacementType { LRU, FIFO, Random };
enum class MissType        { Hit, Compulsory, Capacity, Conflict };

struct CacheLine {        // one way / block frame
    bool     valid = false;
    bool     dirty = false;
    uint64_t tag   = 0;
};

struct CacheSet {
    std::vector<CacheLine> lines;   // size == associativity
};

struct CacheConfig {
    std::string     name;           // "L1", "L2"
    uint64_t        sizeBytes;
    uint64_t        blockSize;
    uint64_t        associativity;
    WritePolicy     writePolicy;
    AllocPolicy     allocPolicy;
    ReplacementType replacement;
    double          hitTime;        // cycles, for AMAT
    uint64_t        addrWidth = 64;
};

struct Stats {
    uint64_t accesses=0, hits=0, misses=0;
    uint64_t reads=0, writes=0, writebacks=0;
    uint64_t compulsory=0, capacity=0, conflict=0;
    double missRate() const { return accesses ? double(misses)/accesses : 0.0; }
    double hitRate()  const { return accesses ? double(hits)/accesses   : 0.0; }
};

class MemoryLevel {       // base so recursion is uniform across caches and memory
public:
    virtual ~MemoryLevel() = default;
    virtual bool access(uint64_t addr, bool isWrite) = 0;  // returns true on hit
    virtual void report(std::ostream&) const {}
};

class Memory : public MemoryLevel {   // terminal: counts traffic, always "hits"
    uint64_t reads_=0, writes_=0;
public:
    bool access(uint64_t, bool isWrite) override { isWrite?writes_++:reads_++; return true; }
};

class Cache : public MemoryLevel {
    CacheConfig cfg;
    uint64_t numSets, offsetBits, indexBits, tagBits;
    std::vector<CacheSet> sets;
    std::unique_ptr<ReplacementPolicy> repl;
    MemoryLevel* next;                       // next level down (another Cache or Memory)
    Stats stats;
    // 3-C support: a parallel fully-associative reference model + a set of seen blocks
public:
    bool access(uint64_t addr, bool isWrite) override;   // the heart — §7.3
    const Stats& getStats() const { return stats; }
};

class CacheHierarchy {
    std::vector<std::unique_ptr<Cache>> levels;  // levels[0]=L1, ...
    Memory mem;
    double memTime;                              // cycles for a memory access
public:
    void   access(uint64_t addr, bool isWrite);  // sends to L1; recursion handles the rest
    void   feed(const Access& a);                // expands a trace record into accesses
    double computeAMAT() const;
    void   reportAll(std::ostream&) const;
};
```

### 6.3 Life of an access (memorize this control flow)

```
Cache::access(addr, isWrite):
    accesses++
    decompose addr → tag, setIndex
    # --- search the set ---
    for each way in set:
        if line.valid and line.tag == tag:        # HIT
            hits++
            repl.onAccess(setIndex, way)
            if isWrite:
                if writeThrough: next.access(addr, write=true)   # forward store
                else:            line.dirty = true               # write-back: mark dirty
            return HIT
    # --- MISS ---
    misses++
    classify(addr)                                # compulsory / capacity / conflict
    if isWrite and allocPolicy == NoWriteAllocate:
        next.access(addr, write=true)             # write-around; nothing cached
        return MISS
    # allocate path: read-miss OR write-miss with write-allocate
    next.access(addr, write=false)                # FETCH the block from below (a read)
    way = pickWay(set)                            # empty way, else victim via repl.getVictim
    if victim is valid and dirty and writeBack:   # evict dirty line
        writebacks++
        next.access(victimAddr, write=true)       # write the victim back
    install(line): valid=true, tag=tag
    if isWrite:
        if writeThrough: { line.dirty=false; next.access(addr, write=true) }
        else            { line.dirty=true }       # write-back store dirties the new line
    else:
        line.dirty = false
    repl.onInsert(setIndex, way)
    return MISS
```

`Memory::access` just tallies and returns hit. AMAT falls out of the per-level miss rates.

---

## 7. Reference implementation (annotated code)

This is the canonical implementation of the bug-prone core. Use it as the guide; the comments explain every non-obvious line.

### 7.1 Trace reader (`trace_reader.h` / `.cpp`)

```cpp
// trace_reader.h
#pragma once
#include <cstdint>
#include <string>
#include <fstream>

enum class Op { Read, Write, Modify, Instr };   // L, S, M, I
struct Access { Op op; uint64_t addr; uint32_t size; };

class TraceReader {
    std::ifstream in_;
public:
    explicit TraceReader(const std::string& path) : in_(path) {}
    bool ok() const { return in_.is_open(); }
    bool next(Access& out);     // returns false at EOF
};
```

```cpp
// trace_reader.cpp
#include "trace_reader.h"

bool TraceReader::next(Access& out) {
    std::string line;
    while (std::getline(in_, line)) {            // stream one line at a time (low memory)
        if (line.empty()) continue;

        char c0 = line[0];
        size_t p = 0;
        Op op;
        if (c0 == 'I') { op = Op::Instr; p = 1; }       // 'I' at column 0 = instruction fetch
        else {
            size_t i = line.find_first_not_of(' ');     // data ops are indented by one space
            if (i == std::string::npos) continue;
            switch (line[i]) {
                case 'L': op = Op::Read;   break;
                case 'S': op = Op::Write;  break;
                case 'M': op = Op::Modify; break;
                case 'I': op = Op::Instr;  break;
                default:  continue;                       // unknown line → ignore defensively
            }
            p = i + 1;
        }

        std::string rest = line.substr(p);                // e.g. " 04ec4af0,4"
        size_t comma = rest.find(',');
        std::string addrStr = (comma==std::string::npos) ? rest : rest.substr(0, comma);
        std::string sizeStr = (comma==std::string::npos) ? "1"  : rest.substr(comma+1);

        out.op   = op;
        out.addr = std::stoull(addrStr, nullptr, 16);     // addresses are HEX (base 16)
        out.size = static_cast<uint32_t>(std::stoul(sizeStr));
        return true;
    }
    return false;
}
```

Key points: we **stream** (never load a multi-GB trace into RAM); the `I` vs indented-`L/S/M` distinction matches the lackey format; `std::stoull(..., 16)` parses hex (parsing as decimal is the #1 trace bug).

### 7.2 Geometry setup (`cache.cpp` constructor)

```cpp
static uint64_t log2u(uint64_t x) {        // assumes x is a power of two
    uint64_t r = 0; while (x > 1) { x >>= 1; ++r; } return r;
}

Cache::Cache(const CacheConfig& c, MemoryLevel* nextLevel) : cfg(c), next(nextLevel) {
    numSets    = cfg.sizeBytes / (cfg.blockSize * cfg.associativity);
    offsetBits = log2u(cfg.blockSize);
    indexBits  = log2u(numSets);                       // 0 when numSets==1 (fully-assoc)
    tagBits    = cfg.addrWidth - indexBits - offsetBits;

    sets.resize(numSets);
    for (auto& s : sets) s.lines.resize(cfg.associativity);   // numSets × assoc frames

    repl = makeReplacement(cfg.replacement, numSets, cfg.associativity);
}
```

Key points: `indexBits` becomes `0` for a fully-associative cache (one set → `setIndex` always 0, which is correct). All line frames are allocated once; nothing allocates on the hot path.

### 7.3 The access function — full (`cache.cpp`)

```cpp
bool Cache::access(uint64_t addr, bool isWrite) {
    stats.accesses++;
    if (isWrite) stats.writes++; else stats.reads++;

    uint64_t blockAddr = addr >> offsetBits;
    uint64_t setIndex  = blockAddr & (numSets - 1);   // low indexBits of blockAddr
    uint64_t tag       = blockAddr >> indexBits;
    CacheSet& set = sets[setIndex];

    // ---------- look for a hit ----------
    for (size_t w = 0; w < set.lines.size(); ++w) {
        CacheLine& line = set.lines[w];
        if (line.valid && line.tag == tag) {           // HIT (valid frame + tag match)
            stats.hits++;
            repl->onAccess(setIndex, w);
            if (isWrite) {
                if (cfg.writePolicy == WritePolicy::WriteThrough)
                    next->access(addr, /*isWrite=*/true);   // forward store
                else
                    line.dirty = true;                      // write-back: mark dirty
            }
            return true;
        }
    }

    // ---------- MISS ----------
    stats.misses++;
    classify(blockAddr);                                // §7.6: compulsory/capacity/conflict

    if (isWrite && cfg.allocPolicy == AllocPolicy::NoWriteAllocate) {
        next->access(addr, /*isWrite=*/true);           // write-around: bypass cache
        return false;
    }

    // allocate path (read miss OR write miss with write-allocate)
    next->access(addr, /*isWrite=*/false);              // FETCH the block (a read from below)

    size_t way = pickWay(set, setIndex);                // empty frame or victim
    CacheLine& line = set.lines[way];
    if (line.valid && line.dirty && cfg.writePolicy == WritePolicy::WriteBack) {
        stats.writebacks++;                             // evicting a dirty line → write it back
        uint64_t vBlock = (line.tag << indexBits) | setIndex;   // rebuild victim's address
        uint64_t vAddr  = vBlock << offsetBits;
        next->access(vAddr, /*isWrite=*/true);
    }

    line.valid = true;                                  // install the fetched block
    line.tag   = tag;
    if (isWrite) {
        if (cfg.writePolicy == WritePolicy::WriteThrough) {
            line.dirty = false;
            next->access(addr, /*isWrite=*/true);       // write-through still forwards
        } else {
            line.dirty = true;                          // write-back: new line is dirty
        }
    } else {
        line.dirty = false;                             // read-fill is clean
    }
    repl->onInsert(setIndex, way);
    return false;
}

size_t Cache::pickWay(CacheSet& set, uint64_t setIndex) {
    for (size_t w = 0; w < set.lines.size(); ++w)       // prefer an empty frame
        if (!set.lines[w].valid) return w;
    return repl->getVictim(setIndex);                   // set full → ask the policy
}
```

Walk the four write/alloc combinations through this code (a favorite drill):

- **WB + WA, write miss:** fetch block; writeback dirty victim if any; install **dirty**.
- **WT + NWA, write miss:** early return → one write straight down, nothing cached. (Standard pairing.)
- **WT + WA, write miss:** fetch; install **clean**; forward store (a read *and* a write down).
- **WB + NWA, write miss:** early return write-around; nothing cached.
- **Read miss (any policy):** fetch; writeback dirty victim if WB; install clean.

Why the victim-address reconstruction matters: when the next level is another cache (L2), the writeback must land in the correct L2 set, which depends on the victim's full block address.

### 7.4 Replacement policies (`replacement.h` / `.cpp`)

```cpp
// replacement.h
#pragma once
#include <cstddef>
#include <vector>
#include <cstdint>
#include <memory>

class ReplacementPolicy {
public:
    virtual ~ReplacementPolicy() = default;
    virtual void   onAccess(size_t set, size_t way) = 0;  // a frame was hit
    virtual void   onInsert(size_t set, size_t way) = 0;  // a frame was filled
    virtual size_t getVictim(size_t set) = 0;             // pick a way to evict
};

std::unique_ptr<ReplacementPolicy>
makeReplacement(ReplacementType t, size_t numSets, size_t assoc);
```

```cpp
// replacement.cpp
#include "replacement.h"
#include <limits>
#include <cstdlib>

// ----- LRU via a monotonically increasing "use clock" -----
class LRUPolicy : public ReplacementPolicy {
    std::vector<std::vector<uint64_t>> lastUsed;   // [set][way] = clock at last use
    uint64_t clock = 0;
public:
    LRUPolicy(size_t s, size_t a) : lastUsed(s, std::vector<uint64_t>(a, 0)) {}
    void onAccess(size_t set, size_t way) override { lastUsed[set][way] = ++clock; }
    void onInsert(size_t set, size_t way) override { lastUsed[set][way] = ++clock; }
    size_t getVictim(size_t set) override {         // LRU = smallest timestamp
        size_t v = 0; uint64_t best = std::numeric_limits<uint64_t>::max();
        for (size_t w = 0; w < lastUsed[set].size(); ++w)
            if (lastUsed[set][w] < best) { best = lastUsed[set][w]; v = w; }
        return v;
    }
};

// ----- FIFO via insertion time only (NOT updated on access) -----
class FIFOPolicy : public ReplacementPolicy {
    std::vector<std::vector<uint64_t>> insertTime;
    uint64_t clock = 0;
public:
    FIFOPolicy(size_t s, size_t a) : insertTime(s, std::vector<uint64_t>(a, 0)) {}
    void onAccess(size_t, size_t) override {}                        // re-access does NOT reorder
    void onInsert(size_t set, size_t way) override { insertTime[set][way] = ++clock; }
    size_t getVictim(size_t set) override {
        size_t v = 0; uint64_t best = std::numeric_limits<uint64_t>::max();
        for (size_t w = 0; w < insertTime[set].size(); ++w)
            if (insertTime[set][w] < best) { best = insertTime[set][w]; v = w; }
        return v;
    }
};

// ----- Random -----
class RandomPolicy : public ReplacementPolicy {
    size_t assoc;
public:
    RandomPolicy(size_t, size_t a) : assoc(a) {}
    void onAccess(size_t, size_t) override {}
    void onInsert(size_t, size_t) override {}
    size_t getVictim(size_t) override { return std::rand() % assoc; }
};

std::unique_ptr<ReplacementPolicy>
makeReplacement(ReplacementType t, size_t s, size_t a) {
    switch (t) {
        case ReplacementType::LRU:    return std::make_unique<LRUPolicy>(s, a);
        case ReplacementType::FIFO:   return std::make_unique<FIFOPolicy>(s, a);
        case ReplacementType::Random: return std::make_unique<RandomPolicy>(s, a);
    }
    return std::make_unique<LRUPolicy>(s, a);
}
```

The one line that distinguishes LRU from FIFO: `FIFOPolicy::onAccess` is **empty**. FIFO evicts by *insertion* order regardless of re-use; LRU updates recency on every hit. That single difference is why LRU usually wins and why FIFO can suffer Belady's anomaly.

Overflow note (interviewers love this): `clock` is `uint64_t`; overflow needs ~1.8×10¹⁹ accesses — physically impossible. So a global counter is safe. If pressed on "what if it did?", answer: rank-compress timestamps periodically, or use a linked-list LRU with no counter.

### 7.5 Hierarchy & AMAT (`hierarchy.cpp`)

```cpp
void CacheHierarchy::access(uint64_t addr, bool isWrite) {
    levels.front()->access(addr, isWrite);   // send to L1; recursion handles L2, memory
}

void CacheHierarchy::feed(const Access& a) {  // translate a trace record into cache accesses
    switch (a.op) {
        case Op::Read:   access(a.addr, false); break;
        case Op::Write:  access(a.addr, true);  break;
        case Op::Modify: access(a.addr, false); access(a.addr, true); break; // load THEN store
        case Op::Instr:  /* ignore for a D-cache study, or route to an I-cache */ break;
    }
}

double CacheHierarchy::computeAMAT() const {
    // Miss penalty of level i == AMAT of everything below it. Build bottom-up.
    double penaltyBelow = memTime;                         // below the last cache is memory
    for (size_t i = levels.size(); i-- > 0; ) {            // iterate L_n .. L1
        const Stats& s = levels[i]->getStats();
        double localMiss = s.missRate();                   // misses / accesses TO this level
        penaltyBelow = levels[i]->cfg.hitTime + localMiss * penaltyBelow;
    }
    return penaltyBelow;                                   // == AMAT the CPU sees
}
```

Why bottom-up works: the *miss penalty* of level *i* is exactly the *AMAT of everything below it*. Start with memory (`memTime`), fold in L2, then L1. `missRate()` here is the **local** miss rate, which is what the recursive formula requires.

### 7.6 3-C classification (`cache.cpp`)

```cpp
void Cache::classify(uint64_t blockAddr) {
    if (!classify3C) return;                          // feature flag (off by default for speed)

    if (seenBlocks.insert(blockAddr).second) {        // .second == true ⇒ first-ever touch
        stats.compulsory++;
        refFA->touch(blockAddr);                      // keep reference cache populated
        return;
    }
    bool refHit = refFA->touch(blockAddr);            // equal-size fully-assoc LRU model
    if (refHit) stats.conflict++;                     // FA would have hit ⇒ blame associativity
    else        stats.capacity++;                     // FA misses too   ⇒ blame capacity
}
```

`refFA` is a tiny fully-associative LRU model (one set, `totalLines` ways) tracking only block addresses (no data, no writes); `touch()` returns whether the block was resident and updates recency. **Drive `refFA->touch()` on every access (hit or miss)** so its LRU state stays faithful — otherwise the capacity/conflict split is wrong.

---

## 8. Build phases with validation gates

Build in this order; each phase is independently testable. Tell Claude Code: *"Implement one phase at a time; after each, run its validation and show the output before continuing."*

- **Phase 0 — Scaffold.** Directory layout, `Makefile`, CLI parsing, `TraceReader` that prints parsed `(op,addr,size)`. *Validate:* it echoes a tiny trace correctly.
- **Phase 1 — One direct-mapped, read-only cache.** Address decomposition + hit/miss counting. *Validate:* the golden example (§11) gives exactly 1 hit / 5 misses.
- **Phase 2 — Associativity + replacement.** Generalize to set- and fully-associative; add `ReplacementPolicy` (LRU, then FIFO, Random) via Strategy. *Validate:* higher associativity lowers misses on a conflict-heavy trace; LRU vs FIFO diverge on a crafted trace.
- **Phase 3 — Writes.** Stores, dirty bits, all four write/alloc combinations, write-back counting. *Validate:* write-through forwards every store; write-back writes only on dirty eviction; never `dirty && !valid`.
- **Phase 4 — Hierarchy.** Chain L1→L2→Memory via the `next` pointer + recursion; expand `M` ops. *Validate:* `L2.accesses == L1.misses` (read-allocate path); local vs global miss rates reported; AMAT matches a hand calc.
- **Phase 5 — Metrics & 3 C's.** Parallel fully-associative reference cache for capacity/conflict; AMAT with configurable hit times + mem time. *Validate:* `compulsory == distinct blocks`; `compulsory+capacity+conflict == misses`; golden trace = 4 compulsory + 1 conflict.
- **Phase 6 — Real traces + report.** `--json` output, `scripts/gen_trace.sh` (Valgrind wrapper), `scripts/sweep.py` (config grid → `reports/comparison.csv` + `.md`, optional plots). *Validate:* a real trace runs end-to-end; report shows ≥3 experiments.
- **Phase 7 — Stretch (optional, pick 1–2).** Next-line prefetcher, victim cache, inclusive/exclusive hierarchy, PLRU, separate I/D caches, OPT/Belady lower bound, `make test` suite.

---

## 9. Directory structure

```
cache-sim/
├── Makefile
├── README.md
├── docs/
│   └── SPEC.md               # this file
├── include/
│   ├── cache.h
│   ├── replacement.h
│   ├── memory.h
│   ├── hierarchy.h
│   ├── trace_reader.h
│   ├── config.h
│   └── stats.h
├── src/
│   ├── cache.cpp
│   ├── replacement.cpp
│   ├── memory.cpp
│   ├── hierarchy.cpp
│   ├── trace_reader.cpp
│   ├── config.cpp
│   └── main.cpp
├── traces/
│   └── tiny.trace            # golden hand-traced test (see §11)
├── configs/
│   └── default.cfg
├── scripts/
│   ├── gen_trace.sh          # valgrind --tool=lackey --trace-mem=yes wrapper
│   └── sweep.py              # config-grid runner → comparison report
├── reports/                  # generated CSV / Markdown / plots
└── tests/
    └── test_cache.cpp        # golden + invariant tests
```

---

## 10. Command-line interface

```
./cachesim --trace traces/tiny.trace \
           --l1-size 1024 --l1-assoc 2 --l1-block 16 \
           --l1-write back --l1-alloc allocate --l1-repl lru \
           --l2-size 16384 --l2-assoc 8 --l2-block 64 \
           --mem-time 100 --l1-hit 1 --l2-hit 10 \
           --report reports/run.md --classify-3c
```

Also accept a config file (`--config configs/default.cfg`) with the same keys, and a `--json` flag that prints a machine-readable stats blob for the sweep script.

---

## 11. Golden validation example

Put this in `traces/tiny.trace` and assert it in `tests/test_cache.cpp`.

Config: **direct-mapped, blockSize = 4 B, 4 sets** ⇒ total 16 B. 8-bit addresses ⇒ `offsetBits=2, indexBits=2, tagBits=4`. All reads.

Access sequence (byte addresses): `0, 4, 8, 0, 16, 0`

| # | addr | block (`addr>>2`) | set (`blk&3`) | tag (`blk>>2`) | result | 3C |
|---|------|------|------|------|--------|----|
| 1 | 0  | 0 | 0 | 0 | MISS | compulsory |
| 2 | 4  | 1 | 1 | 0 | MISS | compulsory |
| 3 | 8  | 2 | 2 | 0 | MISS | compulsory |
| 4 | 0  | 0 | 0 | 0 | HIT  | — |
| 5 | 16 | 4 | 0 | 1 | MISS | compulsory (new block) — evicts set0/tag0 |
| 6 | 0  | 0 | 0 | 0 | MISS | **conflict** (would hit in a 16 B fully-assoc cache) |

**Expected:** accesses=6, hits=1, misses=5 (compulsory=4, conflict=1, capacity=0), hit rate ≈ 16.7%.

Why #6 is *conflict* not *capacity*: distinct blocks touched = {0,1,2,4} = 4 = capacity of a 16 B / 4 B fully-associative cache, so block 0 is still resident there → limited associativity (not size) caused the miss. This single test catches the most common bug (wrong index/tag bit math).

---

## 12. Real-trace generation

On Linux with Valgrind installed (`scripts/gen_trace.sh`):

```bash
# Produce a memory-access trace of any program, then keep only data accesses:
valgrind --tool=lackey --trace-mem=yes ./your_program 2> raw.trace
# raw.trace lines look like:  I 0400d7d4,8 / L 04f6b868,8 / S 7ff0005c8,8 / M 0421c7f0,4
grep -E '^[ ]?[LSM]' raw.trace > traces/program.trace      # drop I lines for a D-cache study
```

Good trace candidates: matrix multiply (great spatial-locality story), linked-list traversal (pointer chasing, poor locality), `gzip`/`grep` on a file, a sorting program. Keep 1–50M lines.

If you can't run Valgrind, write a tiny C program with known access patterns (sequential array scan vs. strided vs. random) and emit the trace yourself — you then *control the locality* and can predict results, which is excellent for both the report and the interview.

---

## 13. Comparison report (what makes this uncommon)

`scripts/sweep.py` runs a config grid and tabulates. Experiments to include in `reports/comparison.md`:

1. **Associativity sweep** (DM, 2, 4, 8, full) at fixed size → miss rate + AMAT. Expect diminishing returns; name the knee.
2. **Block-size sweep** (16/32/64/128 B) → U-shaped miss-rate curve; identify the sweet spot; explain pollution at large blocks.
3. **Capacity sweep** (4/8/16/32/64 KB) → miss rate falls then flattens (working-set effect).
4. **Replacement comparison** (LRU vs FIFO vs Random) → usually LRU ≥ FIFO; show a trace where FIFO exhibits **Belady's anomaly**.
5. **Write-policy traffic** (write-back vs write-through) → write-back drastically lowers write traffic; quantify it.
6. **Locality contrast** (matmul vs linked-list trace) → same cache, wildly different hit rates; tie back to spatial/temporal locality.

For each: a CSV row + a Markdown table, optionally a matplotlib chart in `reports/`. Write a one-paragraph finding per experiment tied to *why* (locality, the 3 C's, AMAT terms). **Your headline numbers come from here.**

---

## 14. Definition of done

- All four write/alloc combinations work; dirty eviction triggers write-backs.
- 3-C classification passes the golden test and `compulsory == distinct blocks`.
- 2-level hierarchy: `L2.accesses == L1.misses`; AMAT matches a hand calculation.
- Real trace runs end-to-end; comparison report generated with ≥3 experiments.
- `make` builds clean with `-Wall -Wextra`; golden + invariant tests pass.
- `README.md` documents build, run, CLI, trace generation, and shows one sample report.

---

## 15. Invariants (your correctness argument)

Assert these throughout — being able to recite them *and explain why each holds* means you can defend the whole project.

1. `hits + misses == accesses` at every level.
2. `compulsory + capacity + conflict == misses` (when 3-C is on).
3. A line is never `dirty && !valid`.
4. For LRU, increasing size or associativity never *increases* misses (the **stack property**). If it does → bug. On FIFO an increase *can* happen — **Belady's anomaly** — and that is correct.
5. `L(n+1).accesses == L(n).misses` for the read-allocate flow (adjust and explain for write-through forwarding / no-write-allocate).

---

## 16. Resume bullet & headline talking points

**Resume bullet template (fill in your measured numbers from §13):**

> *Built a multi-level cache simulator in C++17 (≈2k LOC) supporting configurable associativity, 3 replacement and 4 write policies, with 3-C miss classification and AMAT modeling; ran real Valgrind-generated traces and showed that moving L1 from direct-mapped to 4-way reduced miss rate from \_\_% to \_\_% and AMAT by \_\_%.*

**Talking points to have ready:**

- Derive `numSets = size / (block × assoc)` and the offset/index/tag split on demand.
- "Life of a memory access" walk-through (§6.3) — hit path, miss path, eviction, writeback.
- Local vs global miss rate, and why AMAT uses the local rate.
- The 3 C's, how you classify each, and how to reduce each.
- Why per-set O(ways) LRU here vs. the O(1) hash-map + DLL LeetCode LRU — an engineering trade-off.
- Belady's anomaly and the stack property (why LRU is immune, FIFO isn't).
- The bug your golden test caught (wrong index/tag bit math) and how the invariants guard correctness.
