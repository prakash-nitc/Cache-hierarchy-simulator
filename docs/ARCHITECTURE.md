# Architecture — the concrete design this project follows

> [SPEC.md](SPEC.md) is the authoritative specification. This document is the **map**:
> it explains, in plain language, what every module is, why it exists, how one memory
> access flows through the system, and which build phase creates which piece. Read
> this when you're lost; read the phase docs (`docs/phases/`) to learn the concepts
> behind each part.

---

## 1. The bird's-eye view (30 seconds)

The simulator is a **pipeline with three stages**:

```
   INPUT                    MODEL                       OUTPUT
┌────────────┐      ┌──────────────────┐      ┌─────────────────────┐
│ trace file │ ───► │  cache hierarchy │ ───► │ numbers: hit rates, │
│ (recorded  │      │  (L1 → L2 → mem) │      │ miss types, AMAT,   │
│  memory    │      │  answers HIT or  │      │ CSV + Markdown      │
│  accesses) │      │  MISS per access │      │ comparison report   │
└────────────┘      └──────────────────┘      └─────────────────────┘
```

A recorded list of memory addresses goes in; each address is asked "is this in the
cache?"; statistics about the answers come out. That's the whole system. Everything
below is just organizing that idea into clean, testable parts.

---

## 2. The layered architecture

Five layers, each depending only on the layer below it:

```
┌───────────────────────────────────────────────────────────────────┐
│  LAYER 5 · TOOLING (Phase 6)                                      │
│  scripts/gen_trace.sh   scripts/sweep.py                          │
│  make real traces       run many configs → reports/comparison.md  │
├───────────────────────────────────────────────────────────────────┤
│  LAYER 4 · APPLICATION (every phase)                              │
│  src/main.cpp — CLI parsing, wiring, the trace-replay loop        │
├───────────────────────────────────────────────────────────────────┤
│  LAYER 3 · ORCHESTRATION (Phase 4–5)                              │
│  CacheHierarchy — owns L1, L2, Memory; expands M ops;             │
│  computes AMAT; StatsReporter — console/CSV/JSON output           │
├───────────────────────────────────────────────────────────────────┤
│  LAYER 2 · CORE MODEL (Phase 1–5)             ← the heart         │
│  Cache — decode, hit/miss, eviction, write policies               │
│  ReplacementPolicy — LRU / FIFO / Random (Strategy)               │
│  Memory — terminal level, always "hits", counts traffic           │
│  RefFACache — parallel model for 3-C miss classification          │
├───────────────────────────────────────────────────────────────────┤
│  LAYER 1 · INPUT (Phase 0)                                        │
│  TraceReader — streams (op, address, size) records from disk      │
└───────────────────────────────────────────────────────────────────┘
```

**Why layers?** Each is independently testable: the TraceReader was validated before
any cache existed (Phase 0); the Cache is validated against a hand-traced golden file
before hierarchy exists (Phase 1); the hierarchy is validated with the invariant
`L2.accesses == L1.misses` before reports exist. A bug is always localized to one layer.

---

## 3. Module responsibility table

| Module | Files | One-sentence job | Phase |
|---|---|---|---|
| `TraceReader` | `trace_reader.{h,cpp}` | Turn one line of trace text into a typed `(op, addr, size)` record, streaming, never loading the file whole. | 0 |
| `CacheConfig` | `config.h` | Hold one cache's knobs (size, block, associativity, policies, hit time). | 1→ |
| `Stats` | `stats.h` | Count accesses/hits/misses (later: write-backs, 3-C split) per cache. | 1→ |
| `Cache` | `cache.{h,cpp}` | The core: decode address → tag/index/offset, answer hit/miss, evict, honor write policies, forward misses to the level below. | 1→ |
| `ReplacementPolicy` | `replacement.{h,cpp}` | Decide which way to evict when a set is full (LRU/FIFO/Random behind one interface). | 2 |
| `MemoryLevel` | `memory.h` | Tiny base class so a Cache's "next level" can be *either* another Cache *or* main Memory. | 3–4 |
| `Memory` | `memory.{h,cpp}` | Terminal level: always "hits", tallies read/write traffic. | 3–4 |
| `CacheHierarchy` | `hierarchy.{h,cpp}` | Own L1→L2→Memory, feed trace records in (expanding `M` into load+store), compute AMAT bottom-up. | 4–5 |
| `RefFACache` | inside `cache.cpp` | Equal-size fully-associative LRU shadow model that classifies each miss as compulsory/capacity/conflict. | 5 |
| Reporter / `--json` | `main.cpp` (+ helpers) | Human table on console, machine-readable JSON/CSV for the sweep script. | 5–6 |
| `sweep.py`, `gen_trace.sh` | `scripts/` | Generate real Valgrind traces; run a config grid; write `reports/comparison.{csv,md}`. | 6 |

---

## 4. The two design patterns that carry the whole project

You should be able to name and defend both in an interview.

### 4.1 Recursive composition — the `next` pointer

Every storage level implements one interface:

```cpp
class MemoryLevel {
public:
    virtual bool access(uint64_t addr, bool isWrite) = 0;  // true = hit
};
```

A `Cache` holds a `MemoryLevel* next`. When it misses, it just calls
`next->access(...)` — **it does not know or care** whether "next" is an L2 cache, an
L3, or main memory. Memory is the terminal implementation that always returns true.

```
CPU ──► L1.access()
            │ miss?
            ▼
        L2.access()          each box only knows about the box below it
            │ miss?
            ▼
        Memory.access()      ← always "hits"; recursion bottoms out
```

**Why it's the right design:** adding a third cache level, a victim cache, or a
prefetcher is *insertion into a linked list*, not surgery on existing code. It also
mirrors real hardware, where each level genuinely doesn't know what's behind its
backside bus. This is the Composite/Chain-of-Responsibility idea applied to memory.

### 4.2 Strategy pattern — replacement policies

```cpp
class ReplacementPolicy {
public:
    virtual void   onAccess(size_t set, size_t way) = 0;  // a line was hit
    virtual void   onInsert(size_t set, size_t way) = 0;  // a line was filled
    virtual size_t getVictim(size_t set) = 0;             // whom do we evict?
};
```

The `Cache` calls these three hooks at fixed points and never asks *which* policy is
installed. LRU, FIFO, and Random are drop-in classes behind this interface — the
entire difference between LRU and FIFO is one method body (`onAccess` updates recency
for LRU, does nothing for FIFO).

**Why it's the right design:** cache logic (correctness-critical, bug-prone) is
written once and never touched again when policies are added. Policy code
(simple, swappable) can't corrupt cache state — it only ever returns a way index.

---

## 5. Life of one access (the flow to memorize)

What happens when the CPU reads address `A` (full detail in SPEC §6.3):

```
                       ┌──────────────────────────────┐
 trace line " L A,4" ─►│ TraceReader.next()           │  Layer 1
                       └──────────────┬───────────────┘
                                      ▼
                       ┌──────────────────────────────┐
                       │ Hierarchy.feed(): op==L      │  Layer 3
                       │ → access(A, isWrite=false)   │
                       └──────────────┬───────────────┘
                                      ▼
      ┌────────────────────── L1.access(A) ──────────────────────┐
      │ 1. decode A → offset | setIndex | tag                    │
      │ 2. scan the set's ways:  valid && tag match?             │
      │      YES → HIT: update policy recency; done. ────────────┼──► stats
      │      NO  → MISS:                                         │
      │ 3.   fetch block from below:  next->access(A, read) ─────┼──► recurses into L2,
      │ 4.   pick a frame: empty way, else policy.getVictim()    │    which repeats
      │ 5.   victim dirty (write-back)? → write it back below    │    steps 1–7 for
      │ 6.   install: valid=1, tag=tag, dirty per write policy   │    itself
      │ 7.   policy.onInsert(); done.                            │
      └───────────────────────────────────────────────────────────┘
```

Writes add one decision at step 2 (write-through forwards the store; write-back sets
the dirty bit) and one at step 3 (no-write-allocate bypasses the fill entirely).
Phases 1→3 build this box up incrementally; Phase 4 makes the recursion real.

---

## 6. Data structures at a glance

```
Cache
 ├─ cfg        : CacheConfig          (the knobs)
 ├─ geometry   : numSets, offsetBits, indexBits, tagBits   (derived once)
 ├─ sets_      : vector<CacheSet>     ── numSets of ──► CacheSet
 │                                          └─ lines : vector<CacheLine>  (assoc ways)
 │                                                        └─ {valid, dirty, tag}
 ├─ repl       : unique_ptr<ReplacementPolicy>   (Strategy; owns per-set metadata)
 ├─ next       : MemoryLevel*         (the level below; not owned)
 └─ stats_     : Stats
```

Key property: **everything is allocated once in the constructor**. The access path
(the code that runs 100M times on a real trace) performs zero heap allocations — it
only indexes into pre-sized vectors. Note what a `CacheLine` does *not* contain:
data bytes. We simulate hit/miss behavior, not contents — the tag + flags are enough,
which is why 100M-access traces run in seconds.

---

## 7. Phase → architecture mapping (what exists when)

| Phase | What gets built | Architectural piece |
|---|---|---|
| 0 | Scaffold, Makefile, CLI, TraceReader | Layer 1 + skeleton of Layer 4 |
| 1 | Direct-mapped read-only cache, golden test | First half of the `Cache` box (decode + hit/miss) |
| 2 | Set/fully-associative + LRU/FIFO/Random | `ReplacementPolicy` Strategy plugs into `Cache` |
| 3 | Writes: dirty bits, 4 write/alloc combos | Rest of the `Cache` box + `MemoryLevel`/`Memory` |
| 4 | L1→L2→Memory chaining, `M` expansion | `CacheHierarchy` (Layer 3); recursion goes live |
| 5 | 3-C classification, AMAT, hit times | `RefFACache` shadow model + metrics |
| 6 | Real traces, `--json`, sweep, report | Layer 5 tooling + reporting |
| 7 | Stretch (prefetcher / victim cache / …) | Optional inserts into the `next` chain |

Each phase ends at a **validation gate** (SPEC §8) — a concrete observable output
that must match before moving on. The invariants (SPEC §15) hold at every phase:
`hits + misses == accesses`; never `dirty && !valid`; `L2.accesses == L1.misses`
(read-allocate); `compulsory + capacity + conflict == misses`.

---

## 8. Design decisions and their one-line defenses

| Decision | Defense (say this when asked "why?") |
|---|---|
| C++17, no frameworks | Manual data layout, fast enough for 100M accesses, shows systems fluency. |
| Trace-driven (not execution-driven) | Reproducible, config-sweepable, decouples workload capture from cache modeling. |
| Streaming reader | Traces are GBs; memory stays O(1) in trace length. |
| Power-of-two geometry enforced at construction | Index/tag extraction by mask/shift is only correct for powers of two; fail loudly, not wrongly. |
| O(ways) LRU scan, not O(1) hash+DLL | Sets have ≤16 ways; scanning 8 counters beats pointer chasing. I can write the O(1) version (classic LeetCode LRU) on demand — it wins when the set is thousands of entries. |
| No data bytes stored in lines | Hit/miss behavior depends only on tags; storing data would 100× memory for zero modeling value. |
| Golden hand-traced test + invariants | Lets me *prove* correctness rather than assert it — the golden test catches index/tag math bugs immediately. |
| Strategy + recursive `next` pointer | New policy or new level = new class, zero edits to validated cache logic. |

---

## 9. How to study this project (for the interview)

1. Read each `docs/phases/phase-N.md` *in order* — theory first, then the code
   walk-through, then self-test with the Q&A.
2. Be able to reproduce §5 (life of an access) on a whiteboard — it is the single
   most likely interview drill.
3. Be able to derive `numSets = size / (block × assoc)` and the tag/index/offset
   split from scratch (phase-1.md teaches the derivation).
4. Memorize the invariants (SPEC §15) — they are your correctness argument.
5. After Phase 6, put *your measured numbers* (miss-rate and AMAT deltas from
   `reports/comparison.md`) on the resume bullet — numbers you produced are the
   difference between "did a project" and "did an experiment".
