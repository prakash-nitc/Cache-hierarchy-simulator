# Phase 4 — The hierarchy: L1 → L2 → Memory, local vs global rates, AMAT

> Goal of this phase: turn one cache into a **chain**. An L2 exists because L1
> misses are expensive; the recursion we prepared in Phase 3 makes chaining almost
> free. With two levels come the metrics that only exist *across* levels: **local vs
> global miss rates** and **AMAT** — plus the expansion of `M` trace records into
> load + store. Gates: `L2.accesses == L1.misses` on a pure-read run, and AMAT
> matching a hand calculation exactly.

---

## 1. Theory & concepts

### 1.1 Why multi-level caches exist (the AMAT argument)

L1 must be small to be fast (~1 cycle, constrained by VIPT and physics). Memory is
~100 cycles. With only those two, every L1 miss pays the full 100:

```
AMAT = 1 + missRate_L1 × 100        e.g. 5% misses → 6.0 cycles
```

An L2 inserts a middle price point — bigger and slower than L1 (say 10 cycles), far
faster than memory. Now an L1 miss usually costs 10, not 100:

```
AMAT = 1 + 0.05 × (10 + missRate_L2_local × 100)
```

If L2 catches 80% of what reaches it: AMAT = 1 + 0.05×30 = **2.5** vs 6.0 — the L2
more than halved effective memory time without touching L1. That arithmetic is the
entire reason hierarchies exist, and this phase makes the simulator compute it.

### 1.2 Local vs global miss rate (the classic confusion — get this cold)

- **Local miss rate** = misses at a level ÷ accesses **that reached that level**.
  L2's local rate looks bad by construction: it only ever sees L1's *rejects* — the
  hard, low-locality residue. A 40–80% L2 local rate is normal and not alarming.
- **Global miss rate** = misses at a level ÷ **all CPU accesses**. This says how
  much of the total workload leaks past that level, and it multiplies down the
  chain: `global_L2 = local_L1 × local_L2`.

Our gate run shows both: L2 local 80% (4 of the 5 accesses it saw missed) but L2
global 66.67% (4 of 6 CPU accesses) — and (5/6)×(4/5) = 4/6 ✓. **AMAT uses local
rates** (each level's penalty term is conditioned on reaching it); capacity planning
uses global.

### 1.3 AMAT bottom-up: the miss penalty of a level IS the AMAT below it

The recursive definition: AMAT(level i) = hit_i + localMiss_i × AMAT(level i+1),
with AMAT(memory) = memTime. So compute from the bottom:

```
penalty = memTime                       # below the deepest cache
for i = deepest .. L1:
    penalty = hit_i + localMiss_i × penalty
AMAT = penalty                          # what the CPU sees
```

Hand-check for the gate (tiny.trace, hits 1/6 at L1, L2 local 4/5):
AMAT = 1 + (5/6) × (10 + (4/5) × 100) = 1 + (5/6) × 90 = **76.00** — the simulator
prints exactly this.

### 1.4 What flows *between* levels (the adjusted invariant)

On a pure-read, write-allocate run, every L1 miss triggers exactly one L2 access
(the block fetch): `L2.accesses == L1.misses` — gate 1, exact.

Writes refine it. L1 sends L2: (a) fetches for allocating misses, (b) **write-backs
of dirty victims**, (c) forwarded stores if L1 is write-through, (d) write-around
stores if no-write-allocate. For our WB+WA write.trace run:
`L2.accesses = L1.misses (4) + L1.writebacks (1) = 5` — and L2's stats show it as
reads=4, writes=1. Being able to *predict* the next level's traffic decomposition
from the previous level's policies is exactly the mastery interviewers probe.

Note where the write-back's address mattered: L1 evicted dirty block 0 while
*accessing* block 4 — the write-back landed in L2's set for **block 0** (the
victim's reconstructed address), not block 4's. Phase 3 built that correctly before
it was observable; this phase is where it became load-bearing.

### 1.5 `M` records become two accesses

A lackey `M` (modify) is a read-modify-write: `x++` loads x, then stores x. The
hierarchy's `feed()` expands it into a load followed by a store to the same address
— two CPU accesses, usually a miss-then-hit pair (the load faults the block in; the
store hits it and, under write-back, dirties it). Counting M as one access would
understate traffic and hit rate simultaneously.

### 1.6 Inclusion — the question we now have standing to answer

With two levels, "is L1's content a subset of L2's?" becomes meaningful. Our
hierarchy is **NINE** (non-inclusive, non-exclusive): L2 happens to hold what it
fetched or received, and nothing enforces either containment (inclusive, Intel
tradition — simplifies coherence snooping: probe only L2) or disjointness
(exclusive, AMD tradition — maximizes unique capacity). Real designs pick one for
multicore reasons we don't model. Know the term NINE — naming your own design's
category precisely is a strong interview move.

---

## 2. Line-by-line walk-through of the key code

### 2.1 Deepest-first construction (`src/hierarchy.cpp`)

```cpp
MemoryLevel* below = &mem_;
for (size_t i = cfgs.size(); i-- > 0; ) {
    levels_[i].reset(new Cache(cfgs[i], below));
    below = levels_[i].get();
}
```

- A `Cache` needs its `next` at construction (Phase 3's invariant: never a cache
  without a level below). So the chain is wired bottom-up: Memory first, then the
  deepest cache pointing at it, ending with L1. After this loop, an access to
  `levels_[0]` recurses the whole chain with **zero** hierarchy involvement — the
  hierarchy never routes traffic; the next pointers do.

### 2.2 `feed()` — trace records to CPU accesses

```cpp
case Op::Modify:
    one(a.addr, false, 'R', vlog);   // the load half
    one(a.addr, true,  'W', vlog);   // then the store half
    break;
```

- The expansion lives here — not in the trace reader (whose job is parsing, not
  semantics) and not in Cache (which models one level, not workloads). Layers again.

### 2.3 `computeAMAT()` — §1.3 verbatim

```cpp
double penalty = memTime_;
for (size_t i = levels_.size(); i-- > 0; ) {
    const Stats& s = levels_[i]->stats();
    penalty = levels_[i]->config().hitTime + s.missRate() * penalty;
}
return penalty;
```

- `s.missRate()` is misses/accesses **of that level** — the local rate, which is
  what the recursion requires. Using global rates here is the classic bug; it
  double-counts the upper level's filtering.

### 2.4 Global rates in `reportAll()`

```cpp
uint64_t total = levels_.front()->stats().accesses;    // all CPU accesses
double global  = double(c->stats().misses) / total;
```

- Global is a *hierarchy-level* concept (it needs "all CPU accesses"), so it's
  computed here, not in `Cache::report` — each class reports only what it can know.

---

## 3. Validation (actual outputs)

**Gate 1 — `L2.accesses == L1.misses` + AMAT hand-calc** (tiny.trace; L1 16 B/4 B DM;
L2 32 B/4 B 2-way; 1/10/100 cycles):

```
L1: accesses=6 hits=1 misses=5    global missRate=83.33%
L2: accesses=5 hits=1 misses=4    global missRate=66.67%   ← (5/6)×(4/5) ✓
Memory: reads=4 writes=0          ← = L2 misses ✓
AMAT = 76.00 cycles               ← = 1 + (5/6)(10 + 0.8×100) ✓
```

L2.accesses = 5 = L1.misses ✓. L2 scores a hit because blocks 0 and 4 — L1's
conflict pair — *coexist* in L2's 2-way set 0: the same access pattern that
thrashes L1 is absorbed one level down. That is the hierarchy story in miniature.

**Gate 2 — M expansion** (` M 0,4` alone): verbose shows `R … MISS` then `W … HIT`;
accesses=2, reads=1, writes=1. ✓

**Gate 3 — writes through two levels** (write.trace, both WB+WA):
L1 unchanged from Phase 3 (2/4/1 wb); L2 accesses=5 = misses(4)+writebacks(1),
seen as reads=4 + writes=1; Memory writes=0 — the dirty block parked in L2, never
evicted. AMAT = 47.67 = 1 + (4/6)(10 + 0.6×100) ✓. Invariants OK in all runs. ✓

**Gate 4 — single-level regression:** golden 1 hit/5 misses unchanged; its AMAT
84.33 = 1 + (5/6)×100 is also a hand-calc match. ✓

---

## 4. Interview questions for this phase

**Q1. Why add an L2 instead of making L1 bigger?**
L1's size is pinned by hit time (VIPT constraint, ~4-cycle target): making it big
makes *every* access slower — AMAT's first term. An L2 leaves the fast common case
untouched and cheapens only the miss path: 1 + m₁(h₂ + m₂·mem) instead of
1 + m₁·mem. Different levels optimize different AMAT terms.

**Q2. L2's local miss rate is 80% — is L2 useless?**
No — that's a base-rate illusion. L2 sees only L1's rejects: 5 hard accesses, of
which it still caught one. Globally it cut memory traffic from 5 fetches to 4, and
AMAT from 84.3 to 76.0. Judge a level by global rate and AMAT delta, never by local
rate alone.

**Q3. Derive `global_L2 = local_L1 × local_L2`.**
An access reaches memory only by missing both levels. Missing L2 is conditioned on
reaching it (= missing L1): P(miss both) = P(miss L1) × P(miss L2 | reached L2) =
local₁ × local₂. The gate numbers: (5/6)(4/5) = 4/6 ✓.

**Q4. Why does AMAT use local rates?**
Because the recursion is conditional: the L2 penalty term is only paid *given* an L1
miss, and within that branch the relevant probability is conditioned on arrival —
the local rate. Global rates already include the arrival probability; using them
would multiply it in twice.

**Q5. When is `L2.accesses == L1.misses` exact, and what breaks it?**
Exact when every L1 miss allocates (fetch = one L2 access) and nothing else flows
down: pure reads under write-allocate. Broken by: dirty write-backs (+1 each),
write-through L1 (every store forwarded), no-write-allocate (store misses go around,
still one L2 access but a *write*, not a fetch). Our write run demonstrates the
first: 5 = 4 + 1.

**Q6. Is your hierarchy inclusive or exclusive?**
Neither — NINE (non-inclusive non-exclusive): containment is incidental, not
enforced. Inclusive (L2 ⊇ L1) simplifies coherence — a snoop that misses L2 can't
be in L1; exclusive maximizes unique capacity. Enforcing either needs back-
invalidation or swap-on-fill machinery — a known extension, unneeded single-core.

**Q7 — deeper follow-up. "Your L2 hit for the exact pattern that thrashed L1. Give the general principle, and when it fails."**
Principle: a lower level with more sets, more ways, or larger capacity re-maps the
upper level's conflict pattern — L1's conflict misses look like *reuse* to L2, which
is precisely the traffic an L2 exploits (also the victim-cache idea in miniature).
It fails when the miss stream has no reuse at all — true capacity/streaming misses
(working set ≫ L2) march through both levels; no lower level converts them to hits,
only prefetching hides them. Distinguishing convertible (conflict/reuse) from
unconvertible (streaming) miss traffic is exactly what the 3-C classifier (Phase 5)
formalizes.

**Q8 — deeper follow-up. "AMAT treats misses as serialized. Name what real hardware does that breaks the model, and what AMAT is still good for."**
Real cores overlap misses: out-of-order execution continues past a load miss,
multiple misses are outstanding at once (MLP/MSHRs), critical-word-first returns the
needed word early, and prefetchers convert latency to bandwidth. So wall-clock
stall time < AMAT's prediction, sometimes dramatically. AMAT remains the right
*comparative* metric: it ranks configurations under identical workloads with a
clean, explainable model — which is exactly what a simulator study needs. Say the
limitation before they ask; it converts a gotcha into credibility.
