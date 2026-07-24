# Phase 6 — Real traces, the sweep harness, and the comparison report

> Goal of this phase: turn the simulator into an **instrument**. Everything before
> answered "does this cache work?"; this phase answers "what have we *learned*?" We
> generate real workloads, add machine-readable output, sweep a grid of
> configurations, and produce `reports/comparison.md` — the document that holds the
> **measured numbers** for your resume. Gate: a real trace runs end-to-end and the
> report shows ≥ 3 experiments with findings tied back to the theory.

---

## 1. Theory & concepts

### 1.1 Why this phase is what makes the project uncommon

A cache simulator that runs one config on one toy input is a course assignment. A
simulator that runs *real program traces* across a *grid of configurations* and
produces a *comparison report with explained findings* is an experiment — and
experiments produce numbers you can defend. The entire value of Phases 0–5 is
realized here: correctness (golden tests), diagnosis (3 C's), and metrics (AMAT) now
combine to answer design questions quantitatively.

### 1.2 Where real traces come from

**On Linux — Valgrind lackey** (`scripts/gen_trace.sh`): Valgrind runs *any* binary
under instrumentation and emits every memory access; we keep the `L`/`S`/`M` lines.
This is the gold standard — genuine addresses from a genuine execution — and it's
what the spec targets.

**Everywhere else — a self-instrumenting generator** (`scripts/tracegen.cpp`, the
SPEC §12 fallback we use since this machine has no Valgrind). It runs real algorithms
over real in-memory data structures and prints the **actual virtual address** of each
element it touches. The subtle strength: because *we* wrote the algorithm, we *know*
its locality, so the simulator's results are predictable — and a prediction that
comes true is far stronger evidence of correctness than a number with nothing to
check against. Four workloads, each a locality archetype:

- **matmul** — naive `i-j-k` dense multiply. Row-major `A` scans (spatial), a hot `C`
  accumulator (temporal), and a *strided* `B` column walk (the locality villain).
- **listwalk** — pointer chasing over a **deliberately shuffled** linked list. Every
  `->next` lands on an unpredictable line; spatial locality ≈ 0. This is why linked
  lists are slow in practice, made visible.
- **seqscan** — a pure sequential sweep: best-case spatial locality.
- **randscan** — uniform random touches: worst case for everything.

A fixed RNG seed makes every trace reproducible.

### 1.3 Why a separate `--json` output

Humans read the aligned report; scripts need to *parse*. Mixing the two is fragile
(scraping formatted text breaks the moment you prettify it). So the simulator speaks
two dialects of the same truth: `reportAll` for eyes, `reportJson` for
`sweep.py`. The JSON carries every counter, both miss rates, the 3-C split, memory
traffic, and AMAT — the complete state of a run in one line. This is the standard
shape of a measurement tool: a stable machine contract underneath a friendly face.

### 1.4 What a good experiment looks like

Each experiment changes **one variable** and holds the rest fixed — the scientific
method applied to silicon. The sweep runs six:

1. **Associativity** (1→full) — isolates conflict misses.
2. **Block size** (16→128 B) — isolates spatial-locality capture.
3. **Capacity** (2→128 KB) — isolates the working-set effect.
4. **Replacement** (LRU/FIFO/Random) — isolates the eviction decision.
5. **Write policy** (WB+WA vs WT+NWA) — isolates write traffic.
6. **Locality** (four workloads, one cache) — isolates the *program's* effect.

The discipline that makes findings trustworthy: after measuring, explain the number
using the machinery you built — especially the 3-C classifier, which tells you
*which kind* of miss moved and therefore *why*.

### 1.5 The findings — and the honesty that makes them defensible

The report reports what happened, not what the textbook predicts. Three highlights
worth understanding cold, because they are exactly where an interviewer will probe:

**Associativity has a hidden structure.** DM→4-way cuts matmul's miss rate
32.39%→25.55% (AMAT −20.5%), all conflict misses — the clean 3-C story. But 4-way
and 8-way are *identical*, then fully-associative collapses conflicts to zero. Why?
matmul's inner loop walks `B` down a column: stride = N×8 = 512 B = 8 blocks. Set
index = blockAddr mod 32, so those 8-block steps cycle through only **4 of the 32
sets** — 64 column blocks pile 16-deep into 4 sets. No 4- or 8-way set can hold 16
blocks; only removing indexing (full associativity) or *hashing* it can. This is the
real-world argument for skewed/hashed cache indexing, discovered from a measurement.

**Capacity is a staircase, not a slope.** Miss rate sits at ~25.6% from 2–16 KB,
cliffs at 32 KB (one matrix fits), and bottoms at 0.15% by 64 KB — which equals the
*compulsory floor* (1537 first-touch blocks / 1,048,576 accesses). Capacity buys
nothing until it clears a whole working set, then buys everything at once. "Buy in
quanta or save the silicon."

**LRU is not always best — and we measured it.** LRU wins on matmul (hot accumulator
it protects) but is the *worst* of the three on listwalk (38.99% vs FIFO 38.68%). A
list larger than the cache, traversed cyclically, is LRU's textbook pathology: it
always evicts the block that will be needed soonest. A candidate who can show a
measured LRU-loses case, and name why, has genuinely understood replacement.

---

## 2. Code walk-through

### 2.1 `reportJson` (`src/hierarchy.cpp`)

One JSON object: a `levels` array (each level's config + every counter + local/global
rates + 3-C split), a `memory` object (traffic), and top-level `memTime`/`amat`.
Windows paths are backslash-escaped (`jsonEscape`) so the `trace` field stays valid
JSON. It's emitted on `--json` *instead of* the human report — a script wants exactly
one machine-readable line, nothing else on stdout. Invariants still run; a violation
still exits non-zero, so bad science can't reach the report.

### 2.2 `tracegen.cpp` — the shuffled list is the interesting part

```cpp
std::shuffle(order.begin(), order.end(), rng);
for (int i = 0; i < nodes; ++i)
    cur.next = &pool[order[(i + 1) % nodes]];   // links follow the permutation
```

Nodes live contiguously in a pool, but the *links* follow a random permutation — so
`->next` jumps unpredictably, exactly like a heap list after real-world allocation
churn. A contiguous list would accidentally have spatial locality and lie about
pointer-chasing cost; the shuffle is what makes listwalk honest.

### 2.3 `sweep.py` — one variable at a time

Pure stdlib. `run(trace, **overrides)` starts from a fixed `BASE` config, applies the
one override the experiment varies, runs the simulator with `--json`, and
`json.loads` the result. Each experiment appends CSV rows *and* a Markdown table plus
a **finding paragraph computed from the actual numbers** (not hardcoded) — so
re-running on new traces rewrites the prose to match. The findings deliberately cite
the 3-C counts to explain *why* each curve bends.

---

## 3. Validation (actual outputs)

```
$ mingw32-make traces        # generate the 4 workloads (~2M accesses total)
$ python scripts/sweep.py     # 32 runs -> reports/comparison.{csv,md}
wrote reports/comparison.csv (32 runs) and reports/comparison.md
```

End-to-end on a real 1.05M-access trace (matmul, 8 KB/64 B/4-way, 3-C on):

```
accesses=1048576 hits=780675 misses=267901  hitRate=74.45%
3C: compulsory=1537 capacity=32383 conflict=233981
AMAT = 26.55 cycles          invariants: OK        (~1.9 s)
```

The report shows all six experiments; the headline the resume uses:
**direct-mapped → 4-way L1 cut matmul miss rate 32.39% → 25.55% and AMAT by 20.5%**,
with the 3-C classifier attributing the gain entirely to conflict misses. Gate met. ✔

---

## 4. Interview questions for this phase

**Q1. Why simulate real traces instead of just reasoning about caches?**
Reasoning gives you the direction of an effect; measurement gives you its *magnitude*
and catches the surprises — like matmul's 4-way/8-way plateau, which pure reasoning
misses. A number you produced, with a mechanism you can explain, is what turns "I
studied caches" into "I measured caches."

**Q2. Your machine has no Valgrind — is a self-generated trace legitimate?**
Yes, and arguably better for a *study*: I run real algorithms over real data
structures and emit their actual addresses, so the behavior is genuine — but because
I control the algorithm, I *know* the expected locality, so every result is a
prediction I can check. Valgrind gives realism; the generator adds falsifiability.
The trace format is identical, so the Valgrind path (`gen_trace.sh`) drops in
unchanged on Linux.

**Q3. Explain the matmul associativity result — why do 4-way and 8-way tie?**
The inner loop walks B down a column with a 512 B stride = 8 blocks. The set index is
blockAddr mod 32, so those strides hit only 4 of the 32 sets, piling 64 column blocks
16-deep into 4 sets. A 4- or 8-way set can't hold 16, so neither helps; only full
associativity (or hashed indexing) drains the pileup. It's a power-of-two-stride
conflict pattern — the reason real caches sometimes use skewed indexing.

**Q4. Your capacity curve is flat, then cliffs. Why not smooth?**
Miss rate tracks *working sets*, which are discrete. Below 32 KB no matrix fits, so
added capacity only relabels misses (capacity↔conflict) without removing them; at
32 KB a matrix fits (a cliff); by 64 KB the whole ~96 KB working set is effectively
resident and only compulsory misses remain (the 0.15% floor). Capacity pays off in
quanta aligned to the program's working sets.

**Q5. You found LRU losing. Doesn't that contradict "LRU is best"?**
"LRU is usually best" is a heuristic, not a theorem. On listwalk — a cycle longer
than the cache — LRU evicts precisely the block returning soonest, so it's worst
(38.99% vs FIFO 38.68%). Recency predicts reuse only when reuse distance fits in the
cache; past that, it's anti-predictive. Measuring the counterexample is the point.

**Q6. Why separate `--json` from the human report instead of scraping text?**
Scraping formatted output couples the script to cosmetic formatting — realign a
column and the parser breaks. A dedicated JSON contract is stable, complete, and
unambiguous; the human report can evolve freely. It's the standard split between a
tool's UI and its API.

**Q7 — deeper follow-up. "Your AMAT uses fixed hit/mem times. How seriously should I take the absolute AMAT numbers, versus the deltas?"**
Take the **deltas** seriously and the absolutes with care. The times (1/10/100) are
representative but not my silicon's, and the model serializes misses (no MLP,
prefetch, or write-buffer overlap — Phase 4's caveat). So an absolute "26.55 cycles"
is a model figure, not a wall-clock prediction. But every config in a sweep runs
under the *same* model, so the *comparisons* — 20.5% AMAT reduction from
associativity, 512× write-traffic from write-back — are robust: the modeling
approximations cancel in the ratio. A study's job is comparison, and comparisons are
exactly what this model gets right.

**Q8 — deeper follow-up. "How do you know the sweep's findings aren't just artifacts of your synthetic workloads?"**
Two defenses. First, each finding is *mechanistically* explained and cross-checked
against the 3-C classifier — the associativity gain is confirmed to be all conflict
misses, so it's not a coincidence of the input. Second, the workloads are locality
*archetypes*, chosen to isolate one behavior each; a real program is a blend of these
modes, so the archetype results compose to predict it. The honest limitation:
absolute miss rates depend on the working-set sizes I chose (N=64 matmul, 4096-node
list), so I quote them as *illustrations of a mechanism*, not universal constants —
and the mechanism (conflict from power-of-two strides, LRU's cyclic pathology) is
workload-independent. Naming that boundary is part of trusting the result.
