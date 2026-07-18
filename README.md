# Cache Hierarchy Simulator

A configurable, multi-level (L1/L2) CPU cache simulator in **C++17**. It reads real
memory-access traces and models cache organizations, replacement and write policies,
miss classification (the 3 C's), and AMAT across the hierarchy.

The authoritative design lives in [docs/SPEC.md](docs/SPEC.md). The concrete
architecture (layers, modules, design patterns, phase mapping) is distilled in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Per-phase teaching notes live in
[docs/phases/](docs/phases/).

> **Build status:** Phase 5 complete — 3-C miss classification (compulsory /
> capacity / conflict) via an equal-size fully-associative reference model.

---

## Build

```sh
mingw32-make        # Windows / MinGW (this machine)
make                # Linux / macOS
```

Flags used: `-std=c++17 -Wall -Wextra -O2`. The build is warning-clean.

Other targets:

```sh
mingw32-make run    # build, then echo the golden trace (traces/tiny.trace)
mingw32-make clean  # remove build/ and the binary
```

### Toolchain notes

- This machine uses **MinGW GCC 6.3.0**, invoked as `g++`, with **`mingw32-make`**
  (there is no `make` on `PATH`). Run the build from a shell that provides
  `mkdir`/`rm` (e.g. Git Bash) so the Makefile recipes work.
- GCC 6.3.0 accepts `-std=c++17` but implements only a subset of C++17 — it lacks
  structured bindings and `if (init; cond)`. The code deliberately stays within that
  subset, so it also builds on newer compilers.

---

## Run

Run the golden trace through a direct-mapped 16 B / 4 B cache (SPEC §11):

```sh
./cachesim --trace traces/tiny.trace --l1-size 16 --l1-block 4 --addr-bits 8 --verbose
```

Output ends with `accesses=6 hits=1 misses=5  hitRate=16.67% missRate=83.33%`.

Compare replacement policies on the crafted divergence trace:

```sh
./cachesim --trace traces/lru_vs_fifo.trace --l1-size 8 --l1-block 4 --l1-assoc 2 --l1-repl lru   # 2 hits
./cachesim --trace traces/lru_vs_fifo.trace --l1-size 8 --l1-block 4 --l1-assoc 2 --l1-repl fifo  # 1 hit
```

### CLI flags (current)

| Flag | Meaning |
|------|---------|
| `--trace <file>` | Path to a memory-access trace (required). |
| `--l1-size <bytes>` | Total cache capacity in bytes (required). |
| `--l1-block <bytes>` | Block/line size in bytes, a power of two (required). |
| `--l1-assoc N\|full` | Ways per set: `1` = direct-mapped (default), `full` = fully-associative. |
| `--l1-repl P` | Replacement policy: `lru` (default), `fifo`, `random`. |
| `--l1-write W` | Write-hit policy: `back` (default) or `through`. |
| `--l1-alloc A` | Write-miss policy: `allocate` (default) or `no-allocate`. |
| `--l2-size <bytes>`, `--l2-block <bytes>` | Add an L2 with this capacity / block size. |
| `--l2-assoc`, `--l2-repl`, `--l2-write`, `--l2-alloc` | L2 knobs, same values as the L1 forms. |
| `--l1-hit C`, `--l2-hit C`, `--mem-time C` | Hit/access times in cycles for AMAT (defaults 1 / 10 / 100). |
| `--classify-3c` | Tag every miss compulsory/capacity/conflict (all levels; off by default — costs time and memory). |
| `--addr-bits N` | Address width in bits (default 64; affects only the reported tag width). |
| `--verbose` | Echo every CPU access with its L1 decode + HIT/MISS. |
| `--help`, `-h` | Show usage. |

Two-level example (the Phase 4 gate — `L2.accesses == L1.misses`, AMAT = 76.00):

```sh
./cachesim --trace traces/tiny.trace --l1-size 16 --l1-block 4 \
           --l2-size 32 --l2-block 4 --l2-assoc 2 \
           --l1-hit 1 --l2-hit 10 --mem-time 100 --addr-bits 8
```

> `L` is a read, `S` a write, `M` expands into load + store; `I` lines are
> ignored. Every run reports per-level local **and** global miss rates, memory
> traffic, and AMAT, and ends with invariant checks (`hits+misses == accesses`,
> never `dirty && !valid`, and with `--classify-3c`: the 3-C counts sum to misses
> and `compulsory == distinct blocks`).

### Bundled validation traces

| Trace | Purpose |
|-------|---------|
| `traces/tiny.trace` | Golden test (SPEC §11): 1 hit / 5 misses on a 16 B direct-mapped cache. |
| `traces/conflict.trace` | Blocks 0/4 ping-pong: 6 misses direct-mapped → 2 misses at 2-way. |
| `traces/lru_vs_fifo.trace` | Hot-block pattern where LRU (2 hits) beats FIFO (1 hit). |
| `traces/write.trace` | Store miss/hit + dirty & clean evictions: validates all four write/alloc combos (e.g. write-back: 1 memory write vs write-through: 3). |
| `traces/capacity.trace` | Three blocks cycled through a 2-line fully-assoc cache: 3 compulsory + 3 capacity, conflict impossible. One committed trace per miss type (tiny→conflict too). |

---

## Trace format

One operation per line (Valgrind/CMU `lackey` reduced form):

```
[op] [hex address],[size]
```

- `I` in column 0 — instruction fetch (ignored by the data-cache study).
- ` L` — load (read).  ` S` — store (write).  ` M` — modify = a load **then** a store.
- Addresses are **hexadecimal**. A simple `R <hex>` / `W <hex>` fallback is also accepted.

Example (`traces/tiny.trace`, the golden test from SPEC §11 — byte addresses
0, 4, 8, 0, 16, 0, all loads):

```
 L 0,4
 L 4,4
 L 8,4
 L 0,4
 L 10,4
 L 0,4
```

### Generating real traces

On Linux with Valgrind (see SPEC §12; helper script arrives in Phase 6):

```sh
valgrind --tool=lackey --trace-mem=yes ./your_program 2> raw.trace
grep -E '^[ ]?[LSM]' raw.trace > traces/program.trace   # drop I lines for a D-cache study
```

---

## Directory structure

```
cache-sim/
├── Makefile
├── README.md
├── docs/
│   ├── SPEC.md            # authoritative design
│   └── phases/            # per-phase teaching notes
├── include/               # public headers
├── src/                   # implementation
├── traces/                # tiny.trace (golden) + generated traces (gitignored)
└── reports/               # generated CSV / Markdown (kept in git)
```
