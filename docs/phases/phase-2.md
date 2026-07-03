# Phase 2 — Associativity + replacement policies (LRU, FIFO, Random)

> Goal of this phase: fix the direct-mapped cache's biggest weakness. We let each
> set hold **several** blocks (associativity), which immediately creates a new
> question — *when the set is full, which block do we throw out?* — answered by a
> pluggable **ReplacementPolicy** (the Strategy pattern). Validation gates: the
> golden trace still gives 1 hit / 5 misses; a conflict-heavy trace improves from
> 6 misses (direct-mapped) to 2 (2-way); LRU and FIFO produce *different* hit
> counts on a crafted trace.

---

## 1. Theory & concepts

### 1.1 The problem Phase 1 left us with

In a direct-mapped cache every block has exactly **one** frame it can live in. The
golden trace showed the failure mode: blocks 0 and 4 both map to set 0, so they
evict each other even though sets 1–3 sit half-empty. Two hot addresses that
collide will *ping-pong* forever — 100% misses on data that trivially fits in the
cache. These are **conflict misses**: misses caused not by lack of space but by
lack of *placement freedom*.

### 1.2 Associativity: buying placement freedom

The fix is to group frames into **sets of several ways**. A "way" is just one frame
within a set. A block still maps to exactly one *set* (by its index bits), but
inside that set it may occupy *any* way — the cache checks all of them in parallel.

This creates a spectrum, all described by one identity
(`numSets = size / (block × assoc)`):

```
direct-mapped        assoc = 1            block has exactly 1 home   (fast, conflict-prone)
2-way set-assoc      assoc = 2            2 candidate frames
4/8-way set-assoc    assoc = 4..8         ← what real L1/L2 caches use
fully-associative    assoc = totalLines   block can live anywhere    (no conflicts, expensive)
```

The same decode code covers the whole spectrum: fully-associative just means
`numSets == 1`, so `indexBits == 0` and every block lands in set 0 with the whole
block address as its tag. That falls out of Phase 1's code for free — we didn't
change `decode()` at all this phase.

**Why not always fully-associative, then?** Hardware cost. A hit check must compare
the tag against *every way simultaneously* — one comparator per way. 2–8
comparators are cheap; 512 comparators (a fully-associative 32 KB cache) are slow,
hot, and power-hungry. Real designs stop at 4–16 ways because (a) beyond that the
conflict-miss reduction is tiny, and (b) more ways lengthen the **hit time**, which
AMAT (Phase 5) will show is multiplied into *every* access. This diminishing-returns
knee is exactly what the Phase 6 associativity sweep will measure.

### 1.3 The new decision: whom to evict?

Direct-mapped never chose — one frame, no choice. With multiple ways, a miss into a
**full** set must pick a victim, and that choice is pure *prediction*: "which of
these blocks is least likely to be used again soon?" Three classic answers:

- **LRU (least-recently-used)** — evict the way that has gone unused longest. Bets
  on **temporal locality**: recently used ⇒ likely used again. Usually the best of
  the three; needs recency bookkeeping on *every hit*.
- **FIFO (first-in-first-out)** — evict the way that was *loaded* earliest,
  ignoring how recently it was used. Cheaper: bookkeeping only on fills, never on
  hits. Its blind spot: a block loaded early but still hot gets evicted anyway.
- **Random** — evict any way, uniformly. Zero state, trivial hardware (some real
  ARM cores use it). Surprisingly close to LRU on average workloads because it has
  no *systematic* blind spot — but no upside either.

The entire behavioral difference between LRU and FIFO is **one method**: on a hit,
LRU refreshes the way's timestamp; FIFO does nothing. Same data structures, same
victim scan — one empty function body. Our crafted trace (§3, gate 3) makes that
single line produce measurably different hit rates.

Two facts worth knowing early (both get exercised in later phases):

- **The stack property:** for LRU, a bigger (or more associative) cache always
  contains everything the smaller one would — so misses can never *increase* with
  size. FIFO lacks this property; adding capacity can *increase* its misses
  (**Belady's anomaly**). This is why LRU is our correctness invariant #4 and FIFO
  is the interesting counterexample.
- **O(ways) is a choice, not a limitation:** the famous O(1) LRU (hash map +
  doubly-linked list — the LeetCode classic) is the right tool when the "set" has
  thousands of entries. Inside an 8-way cache set, scanning 8 integers is faster
  than chasing list pointers (better cache behavior, no allocations, trivially
  correct). We implemented O(ways) *on purpose* and can write the O(1) version on
  demand — say exactly this in an interview.

### 1.4 Why policies live behind an interface (Strategy pattern)

The cache asks three questions at fixed points — *"this way was hit"*, *"this way
was filled"*, *"whom do I evict?"* — and does not care how they're answered:

```cpp
void   onAccess(set, way);   // hit    → policy may update bookkeeping
void   onInsert(set, way);   // fill   → policy may update bookkeeping
size_t getVictim(set);       // full miss → policy returns a way index
```

Because policies only *observe* and *return an index*, a policy bug can cost
performance but can never corrupt tags or valid bits — correctness stays inside
`cache.cpp`, which was validated in Phase 1 and did not need re-validation beyond
the golden regression. Adding PLRU or Belady's OPT later means adding a class to
`replacement.cpp` and a factory case; `cache.cpp` is untouched.

---

## 2. Line-by-line walk-through of the key code

### 2.1 LRU — a monotonic "use clock" (`src/replacement.cpp`)

```cpp
std::vector<std::vector<uint64_t>> lastUsed;   // [set][way] = clock at last use
uint64_t clock = 0;
void onAccess(size_t set, size_t way) override { lastUsed[set][way] = ++clock; }
void onInsert(size_t set, size_t way) override { lastUsed[set][way] = ++clock; }
```

- One global counter ticks on every touch; the touched frame is stamped with the
  new value. Bigger stamp = more recent. No lists, no reordering — recency is
  reconstructed at eviction time by comparing stamps.

```cpp
size_t getVictim(size_t set) override {
    size_t v = 0; uint64_t best = std::numeric_limits<uint64_t>::max();
    for (size_t w = 0; w < lastUsed[set].size(); ++w)
        if (lastUsed[set][w] < best) { best = lastUsed[set][w]; v = w; }
    return v;
}
```

- The victim is the way with the **smallest** stamp — least recently touched. This
  is the O(ways) scan discussed in §1.3: at assoc ≤ 16 it beats fancier structures.
- Overflow? `uint64_t` wraps after ~1.8×10¹⁹ ticks. At a billion accesses per
  second that is ~584 years — physically unreachable, so no wrap handling needed
  (know this number; interviewers ask).

### 2.2 FIFO — the same machinery minus one line

```cpp
void onAccess(size_t, size_t) override {}       // re-access does NOT reorder
void onInsert(size_t set, size_t way) override { insertTime[set][way] = ++clock; }
```

- `onAccess` is deliberately **empty** — that emptiness *is* FIFO. Eviction order
  is fixed at fill time; hits don't refresh anything. Everything else (stamp
  array, smallest-stamp victim scan) is identical to LRU.

### 2.3 Random — no state at all

```cpp
size_t getVictim(size_t) override {
    return static_cast<size_t>(std::rand()) % assoc;
}
```

- We intentionally do **not** seed `rand()`, so runs are reproducible on a given
  platform (unseeded `rand()` always starts from the same sequence — a feature for
  a simulator, where you want two runs of the same config to agree).

### 2.4 The cache's side of the contract (`src/cache.cpp`)

The hit path gained one line:

```cpp
if (line.valid && line.tag == d.tag) {
    stats_.hits++;
    repl_->onAccess(d.setIndex, w);      // tell the policy this way was re-used
    return true;
}
```

The miss path now *chooses* a frame instead of assuming frame 0:

```cpp
size_t way = pickWay(set, d.setIndex);
CacheLine& frame = set.lines[way];
frame.valid = true;
frame.tag   = d.tag;
repl_->onInsert(d.setIndex, way);
```

and `pickWay` encodes a rule that matters for correctness of the *policy's view*:

```cpp
size_t Cache::pickWay(const CacheSet& set, uint64_t setIndex) {
    for (size_t w = 0; w < set.lines.size(); ++w)    // prefer an empty frame
        if (!set.lines[w].valid) return w;
    return repl_->getVictim(static_cast<size_t>(setIndex));  // full → ask policy
}
```

- **Empty frames first:** while a set still has invalid ways, there is nothing to
  evict — evicting a *valid* block while an empty frame exists would manufacture
  misses. `getVictim` is therefore only ever called on a **full** set, which also
  means policies never need to special-case "empty way" (their stamp arrays start
  at 0, but those entries are unreachable until every way has been filled once).

### 2.5 A real bug this phase caught (worth retelling in interviews)

Adding `repl_` to `Cache` changed the class's size. The Makefile rebuilt
`cache.cpp` (its `.cpp` changed) but **not** `main.cpp` (unchanged), so `main.o`
still constructed the *old, smaller* `Cache` layout. The two objects linked fine —
and the binary segfaulted at runtime.

The fix is `-MMD -MP`: the compiler emits a `.d` file per object listing every
header it included, and the Makefile `-include`s them, so touching `cache.h` now
rebuilds every translation unit that saw it. Lesson: **a successful link proves
nothing about header consistency** — C++'s One Definition Rule violations are
undiagnosed by default. This is why real build systems track header dependencies
automatically.

---

## 3. Validation (actual outputs)

**Gate 1 — golden regression (assoc=1 must be bit-for-bit Phase 1):**

```
$ ./cachesim --trace traces/tiny.trace --l1-size 16 --l1-block 4 --addr-bits 8
  accesses=6 hits=1 misses=5  hitRate=16.67% missRate=83.33%        ✔
```

**Gate 2 — associativity removes conflict misses** (`traces/conflict.trace`:
blocks 0 and 4 alternating ×3; both map to set 0 when direct-mapped):

| config | result | why |
|---|---|---|
| direct-mapped | `hits=0 misses=6` | one frame: each access evicts the other block |
| 2-way | `hits=4 misses=2` | both blocks fit in set 0's two ways: 2 cold misses, then all hits |
| fully-assoc | `hits=4 misses=2` | same — no conflicts exist to remove beyond 2-way here |

Six misses → two, purely by adding placement freedom. The two that remain are
**compulsory** (first-ever touches) — no associativity can remove those.

**Gate 3 — LRU vs FIFO diverge** (`traces/lru_vs_fifo.trace`: blocks 0,1,0,2,0,1
into a 2-way single set — block 0 is "hot"):

| # | block | LRU | FIFO | the moment they part ways |
|---|-------|-----|------|---------------------------|
| 1 | 0 | MISS | MISS | cold |
| 2 | 1 | MISS | MISS | cold |
| 3 | 0 | HIT  | HIT  | both have {0,1} |
| 4 | 2 | MISS | MISS | set full — LRU evicts 1 (least recent); FIFO evicts 0 (first in) |
| 5 | 0 | **HIT** | **MISS** | LRU kept the hot block; FIFO threw it away |
| 6 | 1 | MISS | MISS | — |

```
LRU : accesses=6 hits=2 misses=4   (hitRate=33.33%)
FIFO: accesses=6 hits=1 misses=5   (hitRate=16.67%)                  ✔ diverged
```

Access #4 is the whole story: both policies must evict, and FIFO's refusal to
notice that block 0 was *just used* (its empty `onAccess`) costs it the hit at #5.
Random on the same trace: `hits=2 misses=4` (platform-reproducible since unseeded).

---

## 4. Interview questions for this phase

**Q1. Define associativity and place direct-mapped / set-associative / fully-associative on one spectrum.**
Associativity = the number of frames ("ways") a given block may occupy, i.e. the
set size. `numSets = size/(block×assoc)`. Direct-mapped is assoc=1 (one home per
block); fully-associative is assoc=totalLines (numSets=1, any block anywhere);
set-associative is everything between — a block maps to one set by its index bits
but to any way within it. Same decode math covers all three; fully-associative
just has zero index bits.

**Q2. In our conflict trace, why did 2-way drop misses from 6 to 2 — and why can't any associativity drop them below 2?**
Blocks 0 and 4 share set 0 in the direct-mapped config, so each access evicted the
other: 6 conflict-flavored misses. With 2 ways both blocks reside simultaneously —
the ping-pong disappears. The remaining 2 misses are the *first-ever* touches of
blocks 0 and 4 (compulsory misses): the cache cannot hit on data it has never
seen, no matter how associative.

**Q3. What is the exact difference between LRU and FIFO in your code, and why does LRU usually win?**
One method body: LRU's `onAccess` restamps the hit way (`lastUsed=++clock`);
FIFO's `onAccess` is empty. So LRU orders eviction by last *use*, FIFO by fill
time. LRU wins on workloads with temporal locality — a re-used block is protected;
FIFO evicts a hot block if it merely arrived early (our gate-3 access #5 shows
exactly that costing a hit).

**Q4. Why does `pickWay` prefer an invalid frame before consulting the policy?**
Evicting a valid block while an empty frame exists would discard useful data for
no reason and manufacture avoidable misses. It also gives policies a clean
contract: `getVictim` is only called on full sets, so policy code never handles
"empty way" as a case.

**Q5. Your LRU is O(ways) per eviction. Interviewers know an O(1) LRU exists — why didn't you build it?**
The O(1) LRU (hash map + doubly-linked list, the LeetCode classic) pays pointer
chasing, allocation, and code complexity to make *arbitrary-size* recency sets
cheap. A cache set has ≤ 16 ways: scanning 8–16 contiguous integers is a handful
of cache-friendly comparisons — faster in practice and trivially correct. Right
tool for the set size; I can write the O(1) version on demand, and it's what I'd
use for a software cache with thousands of entries.

**Q6. Why would real hardware ever use Random replacement?**
It needs zero state and zero update logic — no bits touched on hits, nothing
scanned on eviction — which saves area and power (several ARM cores shipped it).
It has no *systematic* pathology: adversarial patterns that defeat LRU (e.g.
looping over assoc+1 blocks) don't defeat Random reliably. On average workloads it
trails LRU only modestly; the simulator's Phase 6 sweep can quantify by how much.

**Q7 — deeper follow-up. "Your LRU clock is a single global counter shared by all sets. Is that correct? And what if it overflowed?"**
Correct: victim selection only ever *compares stamps within one set*, and a global
monotonic counter preserves relative order everywhere, so per-set counters would
change nothing observable. Sharing one counter is simpler and cache-friendly.
Overflow: `uint64_t` wraps after ~1.8×10¹⁹ ticks — at 10⁹ accesses/second that's
~584 years, so it's physically unreachable. If an interviewer insists ("suppose it
did wrap"): a wrapped stamp would look ancient and get evicted early — a
performance blip, not a correctness bug. To engineer it away: periodically
rank-compress stamps (renumber each set's ways 0..assoc-1 by order) or switch to a
linked-list LRU that stores order structurally with no counter at all.

**Q8 — deeper follow-up. "State the LRU stack property, prove the intuition, and connect it to Belady's anomaly."**
Stack property: under LRU, the contents of a k-way cache are always a **subset**
of the contents of a (k+1)-way cache on the same access stream (for caches sharing
a set's stream; classically stated for fully-associative). Intuition: LRU ranks
blocks by recency — a strict priority order independent of capacity. A cache of
size k holds exactly the k most-recent blocks, and the k most-recent are by
definition a subset of the k+1 most-recent. Consequence: every hit in the smaller
cache is also a hit in the bigger one ⇒ misses are monotonically non-increasing in
capacity/associativity. FIFO's eviction order depends on *arrival* order, which
capacity changes can reshuffle — the subset relation breaks, and adding capacity
can *increase* misses: Belady's anomaly. That's why invariant #4 ("bigger LRU
never misses more") is a *bug detector* for our LRU but must not be asserted for
FIFO — and Phase 6 will hunt for a live Belady case as an experiment.
