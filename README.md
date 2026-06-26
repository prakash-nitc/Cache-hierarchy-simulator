# Cache Hierarchy Simulator

A configurable, multi-level (L1/L2) CPU cache simulator in **C++17**. It reads real
memory-access traces and models cache organizations, replacement and write policies,
miss classification (the 3 C's), and AMAT across the hierarchy.

The authoritative design lives in [docs/SPEC.md](docs/SPEC.md). Per-phase teaching
notes live in [docs/phases/](docs/phases/).

> **Build status:** Phase 0 (scaffold) complete — CLI + trace reader.

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

```sh
./cachesim --trace traces/tiny.trace
```

### CLI flags (current)

| Flag | Meaning |
|------|---------|
| `--trace <file>` | Path to a memory-access trace (required). |
| `--limit N` | Echo at most `N` records (`0` = unlimited, the default). |
| `--help`, `-h` | Show usage. |

> The full cache-configuration flags (`--l1-size`, `--l1-assoc`, replacement / write
> policies, `--classify-3c`, …) described in `docs/SPEC.md` §10 are added in later
> phases as the corresponding features land.

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
