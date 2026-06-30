# Phase 1 — One direct-mapped, read-only cache

> Goal of this phase: turn each byte address into a cache decision. We build a
> single **direct-mapped** cache, decompose addresses into **tag / index / offset**,
> and count hits and misses. The validation gate is the golden trace from SPEC §11:
> exactly **1 hit / 5 misses**.

---

## 1. Theory & concepts

### 1.1 Why a cache splits every address into three fields

A cache stores data in fixed-size **blocks** (a.k.a. lines) — say 4 or 64 bytes —
because of *spatial locality*: if you touch one byte you'll likely touch its
neighbours, so it's cheaper to move a whole block than single bytes. That single
design choice forces the address decomposition.

Given a byte address, the cache must answer three questions, and each is a field of
the address:

```
 MSB                                            LSB
+----------------------+-----------+------------------+
|        TAG           |   INDEX   |   BLOCK OFFSET   |
+----------------------+-----------+------------------+
   tagBits                indexBits     offsetBits
```

- **offset** — *which byte inside the block?* It spans `offsetBits = log2(blockSize)`
  low bits. We don't use it for hit/miss (we cache whole blocks), but it's why the
  low bits are stripped before indexing.
- **index** — *which set does this block map to?* The next `indexBits =
  log2(numSets)` bits. In a direct-mapped cache each set holds exactly one block, so
  the index alone decides the single frame a block can live in.
- **tag** — *which block is actually resident here?* Many block addresses map to the
  same set; the tag (the remaining high bits) disambiguates them. A **hit** is
  `valid && storedTag == tag`.

Crucially, because every quantity (block size, set count) is a power of two, we
extract fields with **shifts and masks**, never `%` or `/`:

```
offset    = addr & (blockSize - 1)     // low offsetBits
blockAddr = addr >> offsetBits         // drop the offset; identify the block
setIndex  = blockAddr & (numSets - 1)  // low indexBits of the block address
tag       = blockAddr >> indexBits     // everything above the index
```

Note the index is taken from the **block** address, not the raw address — a common
off-by-shift bug is masking the raw address before shifting off the offset.

### 1.2 Cache geometry

The one identity to internalize (interviewers ask you to derive it):

```
numSets = cacheSize / (blockSize * associativity)
```

For Phase 1, `associativity = 1` (direct-mapped), so `numSets = totalLines =
cacheSize / blockSize`. The golden config is `cacheSize = 16 B`, `blockSize = 4 B`
⇒ `numSets = 4`, and with 8-bit addresses: `offsetBits = 2`, `indexBits = 2`,
`tagBits = 8 − 2 − 2 = 4`.

### 1.3 What "direct-mapped" buys and costs

Direct-mapped is the simplest organization: one comparator, no replacement choice —
a block has exactly **one** home, so on a miss you overwrite whatever was there.
That simplicity is also its weakness: two hot blocks that map to the same set evict
each other forever even while the rest of the cache sits empty. That's a **conflict
miss**, and the golden trace is engineered to produce one (access #6). We *count* it
as a miss now; *classifying* it as conflict is Phase 5.

### 1.4 The golden trace, hand-traced

Config: direct-mapped, 4 B blocks, 4 sets. Accesses (byte addresses): 0, 4, 8, 0, 16, 0.

| # | addr | block = addr≫2 | set = blk&3 | tag = blk≫2 | result | why |
|---|------|----------------|-------------|-------------|--------|-----|
| 1 | 0  | 0 | 0 | 0 | MISS | cold |
| 2 | 4  | 1 | 1 | 0 | MISS | cold |
| 3 | 8  | 2 | 2 | 0 | MISS | cold |
| 4 | 0  | 0 | 0 | 0 | HIT  | block 0 still in set 0 |
| 5 | 16 | 4 | 0 | 1 | MISS | new block; overwrites set 0's tag-0 |
| 6 | 0  | 0 | 0 | 0 | MISS | set 0 now holds tag 1 → conflict |

⇒ accesses = 6, hits = 1, misses = 5, hit rate ≈ 16.7%. This tiny test is the
single best guard against wrong index/tag math: get a shift wrong and these numbers
move immediately.

---

## 2. Line-by-line walk-through of the key code

### 2.1 Geometry setup — `Cache::Cache` (`src/cache.cpp`)

```cpp
numSets_ = cfg_.sizeBytes / (cfg_.blockSize * cfg_.associativity);
offsetBits_ = log2u(cfg_.blockSize);
indexBits_  = log2u(numSets_);                 // 0 when numSets == 1 (fully-assoc)
tagBits_    = cfg_.addrWidth - indexBits_ - offsetBits_;
sets_.resize(numSets_);
for (CacheSet& s : sets_) s.lines.resize(cfg_.associativity);
```

- The geometry identity becomes `numSets_`. `log2u` counts how many low bits a
  power of two occupies, giving the field widths directly.
- We **allocate all frames once** here so the access path never allocates — important
  when the loop runs hundreds of millions of times.
- Guard rails above this (omitted for brevity) reject a zero/oversized cache and
  enforce that `blockSize` and `numSets` are powers of two and that the size divides
  evenly. Those `throw`s turn an impossible geometry into a clear startup error
  instead of silently-wrong results.

### 2.2 Address decomposition — `Cache::decode`

```cpp
d.offset    = addr & (cfg_.blockSize - 1);
d.blockAddr = addr >> offsetBits_;
d.setIndex  = d.blockAddr & (numSets_ - 1);
d.tag       = d.blockAddr >> indexBits_;
```

- A direct transcription of §1.1. `blockSize - 1` and `numSets_ - 1` are all-ones
  masks because both are powers of two. When `numSets_ == 1` (a future
  fully-associative cache), `indexBits_ == 0`, `setIndex` is always 0, and `tag ==
  blockAddr` — the same code still works, which is why we wrote it this way now.

### 2.3 The access path — `Cache::access`

```cpp
stats_.accesses++;
if (isWrite) stats_.writes++; else stats_.reads++;
Decoded d = decode(addr);
CacheSet& set = sets_[d.setIndex];

for (CacheLine& line : set.lines) {            // search the set
    if (line.valid && line.tag == d.tag) {     // HIT = valid frame + tag match
        stats_.hits++;
        return true;
    }
}

stats_.misses++;                               // MISS
CacheLine& frame = set.lines[0];               // direct-mapped: one frame per set
frame.valid = true;
frame.tag   = d.tag;
return false;
```

- The hit test is `valid && tag matches` — **`valid` first**: an unwritten frame has
  a garbage tag that could coincidentally match, so checking `valid` guards against a
  false hit on a cold cache.
- The search loop is written over `set.lines` even though Phase 1 has one line per
  set. That's deliberate: Phase 2 widens sets to `associativity` ways and adds a
  replacement policy to choose the victim; the *hit search* loop is already correct
  and won't change. Only the victim selection (`set.lines[0]` today) gets replaced.
- We don't touch a "next level" because there isn't one yet (no L2, no memory). A
  miss simply installs the block. Fetch/eviction/write-back traffic is Phase 3–4.

### 2.4 Driving it — `main.cpp`

```cpp
if (a.op == Op::Instr) continue;               // data-cache study: ignore I lines
bool hit = cache.access(a.addr, /*isWrite=*/false);   // read-only: every op is a lookup
```

- Phase 1 is read-only, so `L`, `S`, and `M` are all modeled as a single lookup with
  `isWrite=false`. Real write semantics (dirty bits, write-back/-through, allocate
  policies) and the load-then-store expansion of `M` arrive in Phases 3–4.
- `--verbose` prints `blk`, `set`, `tag`, and HIT/MISS per access so the output can
  be diffed against the hand-trace table — exactly how the golden test is checked.

---

## 3. Validation

```sh
mingw32-make run
# or:
./cachesim --trace traces/tiny.trace --l1-size 16 --l1-block 4 --addr-bits 8 --verbose
```

Actual output:

```
#1 addr=0x0 blk=0x0 set=0 tag=0x0 -> MISS
#2 addr=0x4 blk=0x1 set=1 tag=0x0 -> MISS
#3 addr=0x8 blk=0x2 set=2 tag=0x0 -> MISS
#4 addr=0x0 blk=0x0 set=0 tag=0x0 -> HIT
#5 addr=0x10 blk=0x4 set=0 tag=0x1 -> MISS
#6 addr=0x0 blk=0x0 set=0 tag=0x0 -> MISS
L1 (direct-mapped, size=16B, block=4B, sets=4, assoc=1)
  bits: offset=2 index=2 tag=4
  accesses=6 hits=1 misses=5  hitRate=16.67% missRate=83.33%
```

Matches the SPEC §11 table row-for-row: **1 hit / 5 misses**, bits `offset=2
index=2 tag=4`. ✔

---

## 4. Interview questions for this phase

**Q1. Derive `numSets = size / (block × associativity)` from first principles.**
Total capacity is `size` bytes. Each block holds `block` bytes, so the cache has
`totalLines = size / block` frames. Frames are grouped into sets of `associativity`
ways each, so the number of sets is `totalLines / associativity = size / (block ×
assoc)`. Direct-mapped (`assoc = 1`) ⇒ `numSets = totalLines`; fully-associative
(`assoc = totalLines`) ⇒ `numSets = 1`.

**Q2. How do you split an address into tag/index/offset, and why shifts not division?**
`offsetBits = log2(block)`, `indexBits = log2(numSets)`, `tagBits = addrWidth −
indexBits − offsetBits`. Then `offset = addr & (block−1)`, `blockAddr = addr ≫
offsetBits`, `setIndex = blockAddr & (numSets−1)`, `tag = blockAddr ≫ indexBits`.
Because block and set counts are powers of two, masks/shifts are exact and far
cheaper than `%`/`/`; using division would also break the clean field semantics.

**Q3. Why must the index come from the block address, not the raw address?**
The low `offsetBits` select a byte *within* a block and must not influence which set
the block maps to — otherwise two bytes of the same block could land in different
sets. So we strip the offset (`addr ≫ offsetBits`) first, then take the index bits.

**Q4. What exactly makes a hit, and why check the valid bit?**
A hit is `line.valid && line.tag == tag`. The valid bit matters because a freshly
constructed frame has `tag = 0` (or garbage); without `valid`, an access whose tag is
0 would falsely "hit" an empty frame. Valid gates correctness on a cold cache.

**Q5. In the golden trace, why is access #6 a miss?**
Block 0 and block 4 both map to set 0 (`0 & 3 == 0`, `4 & 3 == 0`). Access #5 (block
4) overwrites set 0's resident block 0, so access #6 (block 0) misses. In a
direct-mapped cache the two blocks fight over a single frame — a conflict miss.

**Q6. Your access loop iterates `set.lines`, but a direct-mapped set has one line. Why write it as a loop?**
Forward-compatibility and uniformity: the hit-search is identical for any
associativity, so writing the loop now means Phase 2 only has to change *victim
selection*, not the search. It costs nothing for `assoc = 1` (one iteration).

**Q7 — deeper follow-up. "Does the address width (`--addr-bits`) change your hit/miss results? Prove it."**
No. Hit/miss depends only on the comparison `storedTag == tag`, where `tag =
blockAddr ≫ indexBits`. Two addresses collide in a set iff they share the same
`setIndex` *and* the same `tag`, i.e. the same `blockAddr` bits above the offset —
independent of how many high bits we *label* as "tag". `addrWidth` only sets the
reported `tagBits` width; it never alters which blocks are considered equal. The
golden trace yields 1 hit / 5 misses at `--addr-bits 8` or `64` alike — only the
printed `tag=4` vs `tag=60` changes. (This is also why the simulator is correct for
both 32- and 64-bit traces with the same code.)

**Q8 — deeper follow-up. "You compare full tags. A real hardware cache stores only `tagBits` of tag. Is comparing the full shifted value ever wrong, and what would break if `numSets` weren't a power of two?"**
Comparing the full `blockAddr ≫ indexBits` is equivalent to comparing the
hardware's truncated tag *as long as the index is extracted with a power-of-two mask*:
two block addresses with the same low `indexBits` map to the same set, and their
remaining high bits (the tag, however wide) are compared in full — there's no
information a hardware tag would distinguish that we wouldn't. It would break if
`numSets` weren't a power of two: `blockAddr & (numSets−1)` is only a valid "mod
numSets" when `numSets` is a power of two. With a non-power-of-two set count you'd
need an actual `% numSets`, the bit fields would no longer be contiguous, and tag ≠
`blockAddr ≫ indexBits`. That's exactly why the constructor *enforces* power-of-two
geometry and throws otherwise — turning a subtle correctness bug into a loud startup
error. (Real designs that want non-power-of-two sets use hashed/skewed indexing
precisely to avoid this.)
