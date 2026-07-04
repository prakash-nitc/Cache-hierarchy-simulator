# Cache Hierarchy Simulator

A configurable, multi-level (L1/L2) CPU cache simulator in **C++17**. It reads real
memory-access traces and models cache organizations, replacement and write policies,
miss classification (the 3 C's), and AMAT across the hierarchy.

The authoritative design lives in [docs/SPEC.md](docs/SPEC.md). The concrete
architecture (layers, modules, design patterns, phase mapping) is distilled in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Per-phase teaching notes live in
[docs/phases/](docs/phases/).

> **Build status:** Phase 3 complete — writes: dirty bits, write-back/write-through
> × write-allocate/no-write-allocate, write-back counting, invariant checks.

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
| `--addr-bits N` | Address width in bits (default 64; affects only the reported tag width). |
| `--verbose` | Print the decode + HIT/MISS for each access. |
| `--help`, `-h` | Show usage. |

> The L2 hierarchy, AMAT, and `--classify-3c` described in `docs/SPEC.md` §10 are
> added in later phases. `L` is a read, `S` a write; `M` is modeled as its load half
> until the hierarchy phase expands it into load+store; `I` lines are ignored.
> Every run ends with an invariant check (`hits+misses == accesses`, never
> `dirty && !valid`) and a memory-traffic report.

### Bundled validation traces

| Trace | Purpose |
|-------|---------|
| `traces/tiny.trace` | Golden test (SPEC §11): 1 hit / 5 misses on a 16 B direct-mapped cache. |
| `traces/conflict.trace` | Blocks 0/4 ping-pong: 6 misses direct-mapped → 2 misses at 2-way. |
| `traces/lru_vs_fifo.trace` | Hot-block pattern where LRU (2 hits) beats FIFO (1 hit). |
| `traces/write.trace` | Store miss/hit + dirty & clean evictions: validates all four write/alloc combos (e.g. write-back: 1 memory write vs write-through: 3). |

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
