# Phase 0 — Scaffold: CLI + Trace Reader

> Goal of this phase: stand up the project skeleton (directory layout, Makefile,
> argument parsing) and a **TraceReader** that turns each line of a memory-access
> trace into a typed `(op, address, size)` record. No cache logic yet — we only
> prove the front-end correctly *reads the workload* the rest of the simulator will
> consume.

---

## 1. Theory & concepts

### 1.1 What a cache simulator actually consumes

A cache simulator does not run a program. It replays a **memory-access trace**: a
recording of the addresses a real program touched, in order. This is *trace-driven
simulation*. Its great advantage is reproducibility — the same trace produces the
same result on every machine — and separation of concerns: the messy job of
observing a program's memory behavior (done once, e.g. by Valgrind) is decoupled
from the clean job of modeling a cache (done by us, repeatedly, under many configs).

Each trace record carries the only three things a cache needs to make a decision:

- **op** — is this a read (load), a write (store), a read-modify-write, or an
  instruction fetch? Reads and writes flow through the cache differently (writes
  involve dirty bits and write policies, which arrive in Phase 3).
- **address** — the byte address. Phase 1 will split it into *tag / index / offset*
  to decide which set a block maps to and whether it is resident.
- **size** — how many bytes the access touched. We carry it now; it matters later
  for traffic accounting and for accesses that straddle block boundaries.

### 1.2 The lackey trace format

We target the Valgrind/CMU `lackey` *reduced* form because it is free, real, and
reproducible — any Linux binary can be turned into one. One operation per line:

```
[op] [hex address],[size]
```

The format has one subtle rule that trips people up: **`I` (instruction fetch) sits
in column 0 with no leading space, while data operations `L`/`S`/`M` are indented by
one space.** That single-column distinction is how the format disambiguates an
instruction fetch from a data load without a separate field. Our reader honors it.

| line        | meaning                                    |
|-------------|--------------------------------------------|
| `I 0400d7d4,8` | instruction fetch (column 0)            |
| ` L 7f0004d8,8`| load / read (indented)                  |
| ` S 7f0004e0,8`| store / write (indented)                |
| ` M 04ec4af0,4`| modify = a load **then** a store        |

Two details we get right on purpose:

1. **Addresses are hexadecimal.** Parsing them as decimal is the single most common
   trace bug; it silently shifts every address and corrupts every index/tag. We pass
   base `16` to `std::stoull`.
2. **We stream, never slurp.** Real traces can be hundreds of millions of lines. We
   read one line at a time with `std::getline` so memory stays O(1) in trace length.

### 1.3 Why locality is the backdrop

Caches only help because programs exhibit **temporal locality** (a touched address is
likely touched again soon) and **spatial locality** (nearby addresses are touched
soon). The trace is exactly the evidence of that locality; later phases will measure
how well a given cache geometry captures it. Phase 0 just makes the evidence readable.

---

## 2. Line-by-line walk-through of the key code

### 2.1 `include/trace_reader.h` — the contract

```cpp
enum class Op { Read, Write, Modify, Instr };   // L, S, M, I
struct Access { Op op; uint64_t addr; uint32_t size; };
```

- `Op` is a scoped `enum class`, so values are written `Op::Read` and cannot
  implicitly convert to `int` — that prevents a whole class of mix-ups later when we
  branch on read vs write.
- `addr` is `uint64_t`: addresses can be 64-bit; using a signed or 32-bit type would
  overflow or sign-extend on real traces.

```cpp
class TraceReader {
    std::ifstream in_;
public:
    explicit TraceReader(const std::string& path) : in_(path) {}
    bool ok() const { return in_.is_open(); }
    bool next(Access& out);
};
```

- The constructor opens the file; `ok()` reports whether that succeeded. We keep the
  stream as a member so successive `next()` calls resume where the last left off —
  this *is* the streaming behavior.
- `next(Access&)` returns `false` at EOF. Filling an out-parameter (rather than
  returning an `Access`) lets the caller reuse one record in a tight loop with no
  per-iteration allocation.

### 2.2 `src/trace_reader.cpp` — the parser

```cpp
while (std::getline(in_, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
```

- Read one line. On Windows a trace may end lines with `\r\n`; in text mode the `\n`
  is consumed but a stray `\r` can remain, which would poison the `size` field — so
  we strip a trailing `\r`. Blank lines are skipped.

```cpp
    if (line[0] == 'I') { op = Op::Instr; p = 1; }
    else {
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos) continue;
        switch (line[i]) {
            case 'L': op = Op::Read;   break;
            case 'S': op = Op::Write;  break;
            case 'M': op = Op::Modify; break;
            case 'I': op = Op::Instr;  break;
            case 'R': op = Op::Read;   break;   // fallback "R <hex>"
            case 'W': op = Op::Write;  break;   // fallback "W <hex>"
            default:  continue;                 // unknown op → skip
        }
        p = i + 1;
    }
```

- This encodes the column-0 rule: a leading `I` is an instruction fetch. Otherwise we
  find the first non-blank character and classify it. `R`/`W` give us the simple
  fallback format the spec also allows. Unknown lines are ignored defensively instead
  of crashing — real traces contain occasional junk. `p` marks where the address
  text begins.

```cpp
    std::string rest = line.substr(p);
    size_t comma = rest.find(',');
    std::string addrStr = (comma == npos) ? rest : rest.substr(0, comma);
    std::string sizeStr = (comma == npos) ? ""   : rest.substr(comma + 1);
```

- Split the remainder at the comma into address and size. If there is no comma (the
  `R/W` fallback), `sizeStr` stays empty and we default the size to 1 below.

```cpp
    out.addr = std::stoull(addrStr, nullptr, 16);                 // base 16!
    out.size = sizeStr.empty() ? 1u : (uint32_t)std::stoul(sizeStr);
```

- **Base 16** is the crux: addresses are hex. The `try/catch` around these guards
  against a malformed numeric token (skip the line rather than abort the run).

### 2.3 `src/main.cpp` — CLI + echo loop

```cpp
auto needVal = [&](const char* name) -> std::string {
    if (i + 1 >= argc) { std::cerr << "error: " << name << " requires a value\n"; std::exit(2); }
    return argv[++i];
};
```

- A tiny helper that consumes the next argv token as a flag's value and fails loudly
  if it is missing. Centralizing this keeps the flag-parsing branches one-liners.

```cpp
while (reader.next(a)) {
    if (limit == 0 || n < limit)
        std::cout << "op=" << opName(a.op)
                  << " addr=0x" << std::hex << a.addr << std::dec
                  << " size=" << a.size << "\n";
    /* tally per-op counts */  ++n;
}
```

- Pull records until EOF, echo each one (respecting `--limit`), and keep per-op
  counts for the summary line. `std::hex`/`std::dec` print the address in hex (how we
  think about it) while keeping `size` decimal. We deliberately validate by eye here;
  Phase 1 replaces the echo with real hit/miss accounting.

### 2.4 `Makefile` — how it scales

```make
SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(BUILDDIR)/%.o,$(SRCS))
```

- Sources are discovered with `wildcard`, so adding `cache.cpp`, `replacement.cpp`,
  … in later phases needs **no Makefile edit**. The `ifeq ($(OS),Windows_NT)` block
  appends `.exe` so the Make target name matches what MinGW's linker actually writes
  (otherwise Make relinks every invocation).

---

## 3. Validation

Build and run the golden trace:

```sh
mingw32-make
./cachesim --trace traces/tiny.trace
```

Expected echo: 6 load records (addresses `0x0, 0x4, 0x8, 0x0, 0x10, 0x0`) followed
by `parsed 6 records: 6 read, 0 write, 0 modify, 0 instr`. Note `0x10` — proof the
hex parse is correct (decimal-16 input would have printed `0x10` only if parsed as
hex; a decimal misparse of "10" would read 10, not 16). The actual hit/miss result of
this trace (1 hit / 5 misses) is asserted in Phase 1.

---

## 4. Interview questions for this phase

**Q1. What is trace-driven simulation and why use it instead of running the program?**
A trace-driven simulator replays a pre-recorded list of memory accesses rather than
executing the program. It is reproducible (same input → same result everywhere),
fast to iterate (record once, simulate under hundreds of cache configs), and cleanly
separates *workload capture* from *cache modeling*. The trade-off is that a fixed
trace cannot react to timing — it can't model how a different cache would change the
program's own behavior — but for cache hit/miss studies that's exactly what we want.

**Q2. Why parse addresses with base 16, and what breaks if you don't?**
Trace addresses are hexadecimal. If you parse `"7f0004d8"` as decimal you either get
a wildly wrong number or a parse failure, and every downstream index/tag computation
is corrupted — typically producing plausible-but-wrong hit rates that are hard to
catch. It's the single most common trace bug, which is why the golden test includes
address `16` (`0x10`) to flush it out.

**Q3. Why does the reader stream one line at a time instead of reading the file into a vector?**
Real traces can be hundreds of millions of lines (gigabytes). Loading them whole
would blow memory and add latency before the first access is processed. `std::getline`
keeps memory O(1) in trace length and lets the simulator start immediately — it never
needs more than one line resident.

**Q4. How does the lackey format tell an instruction fetch apart from a data load?**
By column. An `I` in column 0 (no leading space) is an instruction fetch; data
operations `L`/`S`/`M` are indented by one space. Our parser special-cases a
leading `I`, then for everything else finds the first non-blank character. By default
a data-cache study ignores `I` lines (later they can feed a separate I-cache).

**Q5. Why an out-parameter `next(Access&)` returning bool, rather than returning an `Access` or `std::optional<Access>`?**
The bool/out-parameter idiom lets the caller declare one `Access` outside the loop and
refill it each iteration with zero allocation — important on a hot path that may run
hundreds of millions of times. Returning by value risks per-call copies; `optional`
is cleaner but still constructs a new object each time and wasn't needed here.

**Q6. What is an `M` (modify) operation and how will it be handled?**
`M` is a read-modify-write to the same address — e.g. `inc [mem]`. It counts as **two**
accesses: a load followed by a store to that address. Phase 0 just classifies it as
`Op::Modify`; the hierarchy (Phase 4) expands it into `access(addr, read)` then
`access(addr, write)`.

**Q7 — deeper follow-up. An interviewer says: "Your reader silently skips malformed lines. Defend that, and when would it be the wrong choice?"**
Real traces contain occasional noise (truncated final line, tool banners, partial
flushes). Aborting the whole run on one bad line would waste a long simulation, so
skipping is pragmatic and keeps results reproducible. It is the *wrong* choice when
silent skipping could hide a systematic format mismatch — e.g. if every line fails to
parse, you'd happily report "0 accesses" instead of an error. The robust answer is to
skip but **count and surface** skipped lines (and fail if the skip ratio is high), so
noise is tolerated while a wholesale format error is loud. That counter is a natural
Phase-0 hardening I'd add before trusting a new trace source.

**Q8 — deeper follow-up. "On a 64-bit trace, how do you know `uint64_t` and your bit math will be safe later, and what about a 32-bit address space?"**
Addresses are stored in `uint64_t`, which covers any real 64-bit virtual address with
no sign-extension or overflow. The later tag/index/offset extraction uses shifts and
masks on powers of two, all of which are well-defined on unsigned types (no UB, unlike
signed overflow). For a 32-bit trace nothing changes: the high bits are simply zero,
and `tagBits = addrWidth − indexBits − offsetBits` is driven by the configured
`addrWidth`, so the same code handles both. The one thing to watch is that shift
amounts stay `< 64`; that holds because block/set counts are bounded powers of two.
