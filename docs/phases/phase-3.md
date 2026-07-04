# Phase 3 — Writes: dirty bits and the four write/alloc combinations

> Goal of this phase: handle **stores**. Reads only ever *copy* data upward; writes
> create the hard problem — the cache's copy and memory's copy can now *disagree*,
> and the design question is when to reconcile them. The answer is two independent
> policy choices (write hit × write miss), a **dirty bit**, and a new terminal
> `Memory` level that counts the traffic. Validation gates: write-through produces
> exactly one memory write per store; write-back writes only on dirty evictions;
> and no line is ever `dirty && !valid`.

---

## 1. Theory & concepts

### 1.1 Why writes are fundamentally harder than reads

A read miss has one correct behavior: fetch the block, use it. Nothing anywhere
becomes stale. A **write** changes data — and the moment the cache holds a modified
copy, there are *two versions of the truth*: the cache's and memory's. Every write
design decision is really one question: **when does the level below learn about the
new value?** Immediately (simple, chatty) or lazily (efficient, needs bookkeeping)?

### 1.2 Write HIT policy: write-back vs write-through

- **Write-through:** every store updates the cache *and* is forwarded below,
  immediately. Memory is never stale. Cost: every store is memory traffic — a loop
  storing to one variable 1M times sends 1M writes down. (Real write-through designs
  bolt on a *write buffer* so the CPU doesn't wait for each one.)
- **Write-back:** the store updates only the cached copy and sets the line's
  **dirty bit** = "this line is the only correct copy." Memory is updated once — when
  the line is evicted. The 1M-store loop now costs *one* write-back. This is what
  modern caches do.

The dirty bit is the entire correctness mechanism of write-back: on eviction, a
dirty victim **must** be written below (it's the only copy!), while a clean victim is
silently dropped (memory already matches). Lose the dirty bit and you either lose
data (dropped dirty line) or waste traffic (write back everything).

### 1.3 Write MISS policy: write-allocate vs no-write-allocate

Orthogonal question: the store's block isn't cached at all — do we *bring it in*?

- **Write-allocate:** fetch the block (a read from below), install it, then apply
  the store to it. Bets on locality: you'll probably touch this block again soon.
- **No-write-allocate (write-around):** just send the store below; cache nothing.
  Bets against reuse: e.g. a huge `memset` that writes once and never returns.

Classic pairings: **write-back + write-allocate** (both bet *on* reuse) and
**write-through + no-write-allocate** (both bet *against* deferring). We support all
four combinations because the two axes are genuinely independent.

### 1.4 All four combinations on one trace (the hand-trace that is this phase's gate)

`traces/write.trace` on the golden geometry (direct-mapped, 16 B, 4 B blocks — sets
= blk & 3): `S 0, L 0, S 0, L 0x10, S 8, L 0`. Blocks: 0,0,0,4,2,0 — blocks 0 and 4
collide in set 0.

**WB+WA:** S0 miss→fetch+install *dirty*; L0 hit; S0 hit (stays dirty); L16 miss →
victim block 0 is **dirty → write-back**, install clean; S8 miss→install dirty; L0
miss → victim block 4 **clean → dropped**. Totals: hits=2, misses=4, writebacks=1;
memory sees reads=4 (fetches), writes=1 (the one write-back).

**WT+NWA:** S0 miss→**around** (mem write, nothing cached); L0 miss→fetch; S0
hit→forward (mem write); L16 miss→fetch, victim clean; S8 miss→around (mem write);
L0 miss→fetch, victim clean. Totals: hits=1, misses=5, writebacks=0; memory reads=3,
**writes=3 = exactly the number of stores** — the write-through signature.

**WT+WA:** S0 miss→fetch+install *clean*+forward; L0 hit; S0 hit→forward; L16
miss→fetch; S8 miss→fetch+install+forward; L0 miss→fetch. Totals: hits=2, misses=4,
writebacks=0; memory reads=4, writes=3. Note a WT+WA store miss costs a read *and* a
write below.

**WB+NWA:** S0 miss→around; L0 miss→fetch clean; S0 hit→**dirty**; L16 miss→victim
dirty→**write-back**; S8 miss→around; L0 miss→victim clean. Totals: hits=1,
misses=5, writebacks=1; memory reads=3, writes=3 (2 write-arounds + 1 write-back).

All four verified against the simulator bit-for-bit. Walking one of these aloud is a
guaranteed interview drill — practice WB+WA and WT+NWA minimum.

### 1.5 The victim's address must be *reconstructed*

A dirty eviction writes the victim to the level below — at the **victim's** address,
which is not the address being accessed. The line doesn't store its full address
(that's the point of tags!), so we rebuild it:

```
victimBlock = (tag << indexBits) | setIndex      // undo the decode
victimAddr  = victimBlock << offsetBits
```

Why it matters: today the level below is Memory (which ignores addresses), but in
Phase 4 it's an L2 cache — and the write-back must land in the *victim's* L2 set,
which depends on the victim's own address. Getting this wrong is invisible now and
catastrophically wrong later; that's why it's implemented correctly this phase.

### 1.6 The new invariant: never `dirty && !valid`

A dirty invalid line is a contradiction: "modified data that doesn't exist." It can
only arise from a bookkeeping bug (e.g. installing a line without resetting `dirty`,
or invalidating without write-back). `checkInvariants()` sweeps every frame after the
run; `main` fails loudly if it ever trips. Cheap insurance, strong guarantee.

---

## 2. Line-by-line walk-through of the key code

### 2.1 `MemoryLevel` + `Memory` (`include/memory.h`, `src/memory.cpp`)

```cpp
class MemoryLevel {
public:
    virtual bool access(uint64_t addr, bool isWrite) = 0;   // true on hit
    virtual void report(std::ostream&) const {}
};
```

- The recursion interface (SPEC §6.1): a cache's `next_` is a `MemoryLevel*`, so it
  cannot know or care whether Memory or another Cache is below — the same three call
  sites (fetch, write-back, forwarded store) work for both. `Memory::access` just
  counts and returns true: memory always "hits," so the recursion terminates.
- Memory's counters *are* the validation instrument: `writes` must equal the store
  count under write-through, and the write-back count under write-back.

### 2.2 The write-aware hit path (`src/cache.cpp`)

```cpp
if (isWrite) {
    if (cfg_.writePolicy == WritePolicy::WriteThrough)
        next_->access(addr, /*isWrite=*/true);   // forward the store below
    else
        line.dirty = true;                       // write-back: defer until eviction
}
```

- One branch is the entire hit-policy difference. Write-through never sets dirty
  (below is always current); write-back's single flag assignment is what makes the
  eventual eviction expensive — and everything before it free.

### 2.3 Write-around: the early exit

```cpp
if (isWrite && cfg_.allocPolicy == AllocPolicy::NoWriteAllocate) {
    next_->access(addr, /*isWrite=*/true);
    return false;
}
```

- Placed *before* the fetch: a no-write-allocate store miss touches no frame, evicts
  nothing, and tells the replacement policy nothing. The set is exactly as if the
  access never happened — that's what "around" means.

### 2.4 The allocate path, in strict order

```cpp
next_->access(addr, false);                      // 1. FETCH the block (a read)
size_t way = pickWay(set, d.setIndex);           // 2. choose a frame
if (frame.valid && frame.dirty && cfg_.writePolicy == WritePolicy::WriteBack) {
    stats_.writebacks++;                         // 3. dirty victim → write it back
    uint64_t vBlock = (frame.tag << indexBits_) | d.setIndex;
    uint64_t vAddr  = vBlock << offsetBits_;
    next_->access(vAddr, true);
}
frame.valid = true; frame.tag = d.tag;           // 4. install
frame.dirty = /* write-back store: true; else false (+forward if WT) */;
repl_->onInsert(d.setIndex, way);                // 5. tell the policy
```

- Step 3's condition is a three-way AND: `valid` (there *is* a victim), `dirty` (it's
  the only copy), `WriteBack` (write-through lines are never the only copy — even if
  a stale dirty flag existed, WT semantics make the write-back wrong). The address
  reconstruction is §1.5.
- A write-back **store** installs the line *already dirty* — the fetched block was
  modified the instant it arrived. A write-through store installs clean and forwards.
  A read installs clean, always.

### 2.5 The invariant sweep

```cpp
if (sets_[s].lines[w].dirty && !sets_[s].lines[w].valid) { /* violation */ }
```

- O(lines) after the run — negligible cost, and `main` returns a failure exit code on
  violation so a broken simulation can never quietly produce a report.

---

## 3. Validation (actual outputs)

All on `traces/write.trace`, 16 B / 4 B direct-mapped, 8-bit addresses:

| Combo | Cache result | Memory | Gate |
|---|---|---|---|
| WB+WA | hits=2 misses=4 **writebacks=1** | reads=4 **writes=1** | writes == writebacks ✔ |
| WT+NWA | hits=1 misses=5 writebacks=0 | reads=3 **writes=3** | writes == #stores ✔ |
| WT+WA | hits=2 misses=4 writebacks=0 | reads=4 **writes=3** | writes == #stores ✔ |
| WB+NWA | hits=1 misses=5 **writebacks=1** | reads=3 writes=3 | 2 arounds + 1 wb ✔ |

Every run ends `invariants: OK` (hits+misses==accesses; never dirty&&!valid). The
golden all-loads regression is unchanged (1 hit / 5 misses) with a bonus check now
visible: `Memory: reads=5` == L1 misses — the Phase 4 invariant already emerging. ✔

---

## 4. Interview questions for this phase

**Q1. Why do writes need policies at all when reads don't?**
A read never creates disagreement — data is only copied. A write modifies the cached
copy, so cache and memory now differ; the policy pair decides *when* they're
reconciled: immediately (write-through), or at eviction (write-back + dirty bit).

**Q2. What exactly does the dirty bit mean, and what breaks without it?**
Dirty = "this line is the only correct copy of its block." On eviction, dirty ⇒ must
write back; clean ⇒ drop silently. Without it you either write back *every* victim
(correct but wasteful — that's effectively degrading to write-through-at-eviction) or
drop modified data (silent corruption).

**Q3. Walk a write miss through all four combinations.**
WB+WA: fetch, write-back dirty victim if any, install **dirty**. WT+WA: fetch,
install **clean**, forward the store (read + write below). WB+NWA and WT+NWA: store
goes straight below, nothing cached (identical behavior on the miss; they differ on
hits). This table-walk is SPEC §7.3's drill — practice it aloud.

**Q4. Why is a write-through line never dirty?**
Because the level below is updated on every store, the cached copy is never the only
copy. `dirty` would be meaningless — our code keeps it false so eviction always drops
WT lines silently.

**Q5. Why must the victim's address be reconstructed instead of stored per line?**
Storing full addresses per line duplicates what tag+position already encode — pure
waste (the whole point of the tag/index split is that position encodes the index
bits). Reconstruction is two shifts and an OR: `(tag<<indexBits | set) << offsetBits`.
It matters because the write-back must address the *victim's* block, which lands in a
different L2 set than the incoming block once the next level is a cache.

**Q6. Your write-around path skips `repl_->onAccess/onInsert` entirely. Is that right?**
Yes — deliberately. No frame was touched: nothing was inserted, and no cached way was
re-used, so there is no recency information to record. Notifying the policy would
corrupt LRU order with a phantom touch.

**Q7 — deeper follow-up. "Write-back plus no-write-allocate looks self-defeating — the write policy bets on reuse, the alloc policy bets against it. Why support it, and when could it win?"**
The axes are mechanically independent, so supporting all four costs nothing and lets
the simulator *measure* the pairing question instead of assuming it. WB+NWA can win
on a workload whose **stores** are streaming (write-once buffers — never allocate
them) but whose **read-resident** data gets occasional updates: those update stores
hit lines already cached by reads, and write-back absorbs them; allocating the
streaming stores would evict the useful read set. It's rare but real — and "I can
run that experiment" is a stronger answer than any prior. That's the point of
building the tool.

**Q8 — deeper follow-up. "Under write-through, is `Memory.writes == store count` always exact? What about under write-back — can writebacks exceed stores?"**
WT: exact in this simulator — every store hits or misses, and both paths forward
exactly one write (WA forwards after fill; NWA forwards around); nothing else writes
below. Real systems blur it with write buffers *coalescing* adjacent stores (fewer
memory writes than stores). WB: writebacks can never exceed *distinct dirtied lines
evicted*; one write-back can represent thousands of absorbed stores (that's the whole
win), but also a single store can cause at most one eventual write-back — so
writebacks ≤ stores always, with equality only in the pathological one-store-per-
block-then-evict pattern. Being able to bound the counters both ways shows you own
the mechanism, not just the definitions.
