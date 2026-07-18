# Phase 5 — The 3 C's: classifying every miss

> Goal of this phase: stop just *counting* misses and start *diagnosing* them.
> Every miss gets a verdict — **compulsory** (unavoidable first touch), **capacity**
> (the cache is too small), or **conflict** (too few ways) — because each verdict has
> a *different cure*, and a report that says "12% misses, mostly conflict" tells you
> exactly which knob to turn. Gates: golden trace = 4 compulsory + 1 conflict;
> `compulsory == distinct blocks`; the three counters sum to misses.

---

## 1. Theory & concepts

### 1.1 Why classify misses at all

"Miss rate 12%" is a symptom, not a diagnosis. The same 12% could mean: the program
touches lots of new data (compulsory — buy bigger blocks or prefetch), the working
set exceeds the cache (capacity — buy a bigger cache), or hot blocks are fighting
over the same sets (conflict — buy more ways, not more kilobytes). Turning the wrong
knob wastes silicon: doubling a cache's size does *nothing* for compulsory misses,
and adding ways does nothing for capacity misses. The 3-C taxonomy (Hill & Smith)
turns one number into an action.

### 1.2 The three definitions, precisely

- **Compulsory (cold):** the very first access to this block, ever. Even an
  *infinite* cache would miss — the data has simply never been loaded. Detection is
  exact: keep a set of every block address ever touched; a miss on a never-seen
  block is compulsory. Corollary: `compulsory == number of distinct blocks touched`
  — which is why it doubles as an invariant.
- **Capacity:** the block *was* seen before, but even a **fully-associative cache of
  the same total size** (the most flexible possible organization) would have evicted
  it by now — the working set simply doesn't fit in this many bytes.
- **Conflict:** the block was seen before, and the same-size fully-associative cache
  **still holds it** — meaning the bytes were sufficient, and the miss happened only
  because limited associativity forced this block into a crowded set. The miss is
  the *organization's* fault, not the size's.

The instrument that separates the last two is the **reference model**: a shadow,
fully-associative, LRU cache of the *same total capacity*, tracking only block
addresses. On a repeat miss, ask it: "would you have hit?" Yes → conflict; no →
capacity.

### 1.3 The subtle rule: touch the reference model on HITS too

This is the classic 3-C implementation bug, and the reason the phase doc exists.
The reference model is an *LRU* cache — its verdicts depend on its recency order —
and recency is built by **every** access, not just the misses. If you only touch it
on misses, a block the real cache hits repeatedly looks *idle* to the reference
model, ages out of it, and a later miss on that block gets misclassified capacity-
instead-of-conflict (or vice versa). So: real-cache **hit** → `refFA->touch()`
(recency update only); real-cache **miss** → `classify()` (which also touches).
One missing line, silently wrong science — exactly the kind of bug the invariants
can't catch, because the counts still sum correctly. Know this cold; it's the
deepest drill question this phase offers.

### 1.4 Reading the verdicts: cure per C

| Verdict | Meaning | Cure |
|---|---|---|
| compulsory | never seen this data | larger blocks (amortize the first touch over more bytes), prefetching |
| capacity | working set > cache bytes | bigger cache; software: blocking/tiling the algorithm |
| conflict | sets too crowded | more ways, victim cache, hashed/skewed indexing |

Sanity properties worth internalizing: a fully-associative cache has **zero conflict
misses by definition** (it *is* the reference model — gate C shows the classifier
agreeing). An infinite cache has only compulsory misses. And the classification is
*relative to LRU*: the reference model defines "should have fit" as "an LRU policy
would have kept it," which is the standard convention but not gospel (see Q&A 8).

### 1.5 Our gate traces, diagnosed

- **tiny.trace** (golden): 5 misses = 4 compulsory (blocks 0,1,2,4 first touches) +
  1 conflict (the final re-access of block 0: the 4-line FA reference still holds
  all 4 distinct blocks, so only direct-mapping's set-0 crowding explains the miss).
  This is SPEC §11's exact expectation, now machine-verified.
- **conflict.trace** (0/16 ping-pong): 2 compulsory + **4 conflict** + 0 capacity —
  the classifier formally certifies what Phase 2 showed empirically: every repeat
  miss vanishes with 2 ways, because bytes were never the problem.
- **capacity.trace** (blocks 0,1,2 cycled through a 2-line FA cache): 3 compulsory +
  **3 capacity** + 0 conflict — three blocks genuinely don't fit in two lines; no
  organization of 8 bytes fixes it.

Three committed traces, one per C — you can *demonstrate* each miss type on demand.

---

## 2. Line-by-line walk-through of the key code

### 2.1 `RefFACache` (`include/cache.h`, `src/cache.cpp`)

```cpp
class RefFACache {
    std::unordered_map<uint64_t, uint64_t> lastUsed_;   // blockAddr -> use clock
    size_t capacity_;                                    // == cache totalLines
    uint64_t clock_ = 0;
public:
    bool touch(uint64_t blockAddr);
};
```

- Same use-clock idea as `LRUPolicy` — deliberate symmetry: one design, two uses.
  A hash map replaces the per-set arrays because there's a single set of
  `totalLines` ways, and membership lookups must be O(1).
- `touch()` does everything: resident → refresh stamp, return true; absent → evict
  the smallest-stamp entry if full, insert, return false. Eviction is an O(n) scan
  but runs only on reference-model *misses*; hits (the common case) are O(1).
- It stores **no data, no dirty bits, no sets** — just block addresses and stamps.
  The model answers exactly one question: "would an ideally-flexible cache of the
  same size still hold this block?"

### 2.2 `classify()` — the verdict logic

```cpp
if (seen_.insert(blockAddr).second) {   // first-ever touch
    stats_.compulsory++;
    refFA_->touch(blockAddr);           // keep the model populated
    return;
}
if (refFA_->touch(blockAddr)) stats_.conflict++;   // FA would have hit
else                          stats_.capacity++;   // FA misses too
```

- `insert().second` is the elegant bit: one hash-set operation both *tests* and
  *records* first-touch — no separate lookup.
- Order matters: the compulsory branch must touch the model too (the block enters
  the reference cache the same moment it enters the real one), and `classify` is
  called before the write-around early-return so *every* miss gets a verdict.

### 2.3 The one-line hit-path duty (§1.3 made real)

```cpp
if (refFA_) refFA_->touch(d.blockAddr);   // in the HIT path
```

- The line that keeps the science honest. Delete it and every count still sums —
  the invariants stay green — but the capacity/conflict split silently rots. Tests
  can't save you from a plausible wrong number; only understanding the model can.

### 2.4 New invariants

```cpp
compulsory + capacity + conflict == misses      // every miss got exactly one verdict
compulsory == seen_.size()                      // first-touches == distinct blocks
```

- Both checked after every run. The second is the free gift of exact bookkeeping:
  two independently-maintained quantities that must agree — a bookkeeping bug in
  either shows up as disagreement.

### 2.5 Why it's off by default (`--classify-3c`)

Classification adds a hash-set insert per miss and a map touch per access, plus
memory proportional to *distinct blocks in the trace* (the `seen_` set grows without
bound). On a 100M-access trace that's real time and real gigabytes-adjacent memory.
The flag makes the cost opt-in — measurement instruments shouldn't tax the
measurement they're not part of.

---

## 3. Validation (actual outputs)

```
GATE A  tiny.trace, 16B/4B DM:        3C: compulsory=4 capacity=0 conflict=1   ✔ SPEC §11 exact
GATE B  conflict.trace, 16B/4B DM:    3C: compulsory=2 capacity=0 conflict=4   ✔
GATE C  capacity.trace, 8B FA:        3C: compulsory=3 capacity=3 conflict=0   ✔ (FA ⇒ conflict impossible)
GATE D  tiny 2-level, both classified: L1 4/0/1; L2 compulsory=4 capacity=0 conflict=0
        (4 distinct blocks reach L2; its 8-line reference never evicts)        ✔
GATE E  flag off: no 3C line, output byte-identical to Phase 4                 ✔
```

All runs: `invariants: OK` — including the two new checks (3C sum == misses,
compulsory == distinct blocks). AMAT unchanged at 76.00 on the 2-level gate.

---

## 4. Interview questions for this phase

**Q1. Define the 3 C's and give the cure for each.**
Compulsory: first-ever touch — larger blocks or prefetching. Capacity: working set
exceeds total bytes — bigger cache (or software tiling). Conflict: enough bytes but
too few ways for the mapping pattern — higher associativity, victim cache, or
hashed indexing. Each verdict names the knob that actually helps.

**Q2. How do you detect each type mechanically?**
A global seen-blocks set: miss on an unseen block = compulsory. Otherwise consult a
same-size fully-associative LRU reference model: it hits = conflict (organization's
fault); it misses = capacity (size's fault). That's the Hill & Smith method, and
it's exactly my `classify()`.

**Q3. Why must the reference cache have the SAME total size?**
The question it answers is "were the *bytes* sufficient, with perfect flexibility?"
Same size isolates the organization variable: any miss the reference avoids is
attributable purely to associativity. A bigger reference would conflate size and
organization; the comparison would answer nothing.

**Q4. Why is `compulsory == distinct blocks` an invariant?**
Each distinct block is unseen exactly once — its first access, which is by
definition its compulsory miss (whether that access misses is guaranteed: never
seen ⇒ never resident). Two independent counters (verdicts vs. set cardinality)
must agree; divergence = bookkeeping bug.

**Q5. Can a fully-associative cache have conflict misses?**
No — conflict is *defined* as "the same-size FA cache would have hit." When the
real cache *is* FA (and LRU), it behaves identically to the reference, so every
repeat miss is capacity. My capacity.trace gate shows the classifier reproducing
this: 3/3/0. (Caveat: with a non-LRU policy, an FA cache could miss where the LRU
reference hits — see Q8.)

**Q6. Why classify *before* the write-around early return?**
No-write-allocate store misses are still misses of the level — they cost a trip
below and belong in the diagnosis. Classifying after the early-out would silently
drop them and break `3C sum == misses`.

**Q7 — deeper follow-up. "Why must the reference model be touched on hits? Construct the failure."**
The model's verdicts hang on its LRU order, which every access shapes. Touch it
only on misses and hot blocks (hitting constantly in the real cache) look idle to
the model and age out. Concrete failure: DM cache, block A hits repeatedly in set
0 while blocks B,C,D,E miss elsewhere. Proper accounting keeps A at the reference's
MRU end; miss-only accounting lets B–E push A out. When A is finally evicted in the
real cache by a set-0 rival and re-accessed, the correct verdict is conflict (the
bytes held it; the set didn't) — the broken model says capacity. Both worlds show
identical hit/miss counts and pass every sum invariant; only the *diagnosis* is
wrong, which then prescribes the wrong hardware fix. It's my favorite example of a
bug tests can't catch but understanding can.

**Q8 — deeper follow-up. "Your 'conflict' verdict is relative to an LRU reference. Is the taxonomy well-defined? Where does it mislead?"**
It's a *convention*, not a law of nature — Hill & Smith fix the reference as
same-size FA + LRU precisely so numbers are comparable across studies. It misleads
at the edges: (1) LRU pathologies — a cyclic scan slightly bigger than the cache
makes FA-LRU miss everything, so misses get labeled 'capacity' even though a
smarter policy (or even the real cache's mapping — my capacity.trace on a
direct-mapped config actually *hits* once where the FA reference misses!) would do
better; strictly the label means 'capacity under LRU'. (2) Anti-conflict effects:
the real cache can beat the reference, so 'conflict' can even go slightly negative
in principle — implementations clamp it by construction, as mine does via the
if/else. (3) An alternative reference is Belady's OPT (evict the block used
farthest in the future), which gives a true lower bound but needs the future —
it's on my stretch list as an offline pass. Knowing the taxonomy's edges is what
separates using a tool from understanding it.
