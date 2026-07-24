# Comparison report

Workloads: self-instrumented algorithms over real data-structure addresses
(`make traces`): dense **matmul** (mixed locality), shuffled-list
**listwalk** (pointer chasing), **seqscan** (pure spatial), **randscan**
(no locality). Baseline L1: 8 KB, 64 B blocks, 4-way, LRU, write-back +
write-allocate; AMAT times 1 / 100 cycles (single level unless noted).

## 1. Associativity sweep (matmul, 8 KB, 64 B blocks)

| assoc | miss rate | conflict misses | AMAT |
|---|---|---|---|
| 1 | 32.39% | 305671 | 33.39 |
| 2 | 26.17% | 240475 | 27.17 |
| 4 | 25.55% | 233981 | 26.55 |
| 8 | 25.55% | 233981 | 26.55 |
| full | 3.23% | 0 | 4.23 |

**Finding:** direct-mapped → 4-way cuts miss rate 32.39% → 25.55% and AMAT 33.39 → 26.55 cycles (20.5% faster), all of it conflict misses (305671 → 233981) — the 3-C prediction. But note the *shape*: 4-way and 8-way are identical, yet fully-associative collapses conflicts to 0 (3.23% miss rate). The culprit is matmul's B-column walk: its 512 B power-of-two stride maps all 64 column blocks into just 4 of the 32 sets — a 16-deep pileup that no practical way count drains. Only removing indexing entirely (full associativity) fixes it, which is precisely why real designs facing strided workloads use hashed/skewed indexing rather than more ways.

## 2. Block-size sweep (8 KB, 4-way; two contrasting workloads)

| block | seqscan miss rate | matmul miss rate |
|---|---|---|
| 16 B | 50.01% | 25.91% |
| 32 B | 25.01% | 25.65% |
| 64 B | 12.51% | 25.55% |
| 128 B | 6.26% | 25.53% |

**Finding:** block size is a *spatial-locality* knob, and the two workloads prove it in opposite directions. seqscan's miss rate halves with every doubling (50.01% → 6.26%), tracking the 8/block law exactly — one miss loads block/8 sequential words. matmul barely moves (25.91% → 25.53%): its misses are dominated by the B-column stream whose 512 B stride exceeds any tested block, so no block size captures it — the wrong knob for a conflict-dominated workload (experiment 1 showed the right one). The textbook U-curve's rising side (pollution) needs a reuse-rich working set squeezed by oversized blocks; neither pure workload manifests it, and knowing *why* is the point of classifying misses before turning knobs.

## 3. Capacity sweep (matmul, 64 B blocks, 4-way)

| size | miss rate | capacity misses | AMAT |
|---|---|---|---|
| 2 KB | 25.56% | 266492 | 26.56 |
| 4 KB | 25.56% | 266492 | 26.56 |
| 8 KB | 25.55% | 32383 | 26.55 |
| 16 KB | 25.53% | 32383 | 26.53 |
| 32 KB | 2.47% | 4147 | 3.47 |
| 64 KB | 0.15% | 0 | 1.15 |
| 128 KB | 0.15% | 0 | 1.15 |

**Finding:** not a smooth curve but a *staircase*, and each tread is a working set. Flat at ~25.56% from 2–16 KB (the strided B stream misses regardless of capacity — only its 3-C label shifts between capacity and conflict), then a cliff at 32 KB — the size at which one 32 KB matrix fits — and by 64 KB the whole ~96 KB working set is effectively resident: miss rate 0.15% ≈ the compulsory floor (1537 first-touches / 1048576 accesses = 0.15%), and measured capacity misses reach 0. Capacity helps in quanta, not gradients — buy enough for the next working set or save the silicon.

## 4. Replacement policy (8 KB, 4-way, matmul and listwalk)

| trace | LRU | FIFO | Random |
|---|---|---|---|
| matmul | 25.55% | 26.97% | 27.02% |
| listwalk | 38.99% | 38.68% | 38.75% |

**Finding:** on matmul, LRU < FIFO < Random (25.55% vs 26.97% vs 27.02%) — the C accumulator row is hot, LRU protects it, FIFO evicts it on schedule. listwalk *inverts* the order: LRU (38.99%) is slightly WORST (38.68% FIFO, 38.75% Random). That's the classic cyclic pathology: the 64 KB list is one giant cycle through an 8 KB cache, so LRU always evicts exactly the block that will return soonest. When reuse distance exceeds capacity, recency is anti-information — a measured, honest counterexample to 'LRU is always best'.

## 5. Write-policy traffic (matmul, 8 KB, 4-way)

| policy pair | memory writes | writebacks |
|---|---|---|
| write-through + no-allocate | 262144 | 0 |
| write-back + allocate | 512 | 512 |

**Finding:** write-through sends every one of matmul's 262144 stores to memory (262144 writes); write-back coalesces repeated stores to the hot C row into 512 dirty-eviction write-backs — **512× less write traffic**. This is the dirty bit earning its keep.

## 6. Locality contrast (identical cache: 8 KB, 64 B, 4-way, LRU)

| workload | hit rate | AMAT | character |
|---|---|---|---|
| matmul | 74.45% | 26.55 | mixed spatial+temporal |
| seqscan | 87.49% | 13.51 | pure sequential |
| listwalk | 61.01% | 39.99 | pointer chasing |
| randscan | 12.57% | 88.43 | uniform random |

**Finding:** the same silicon spans 12.57% to 87.49% hit rate purely on access pattern. seqscan hits 7 of every 8 accesses mechanically (64 B block = 8 sequential words per fill); listwalk defeats spatial locality by design and lives at memory speed. Hardware doesn't make code fast — locality does; the cache only rewards it.
