#!/usr/bin/env python3
"""sweep.py — run the SPEC section-13 experiment grid and build the comparison report.

Usage:
    python scripts/sweep.py                 # all experiments -> reports/comparison.{csv,md}

Prerequisites: the simulator is built (make) and the workload traces exist
(make traces). Pure stdlib — no third-party dependencies.
"""

import csv
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIM = next((p for p in (ROOT / "cachesim.exe", ROOT / "cachesim") if p.exists()), None)
TRACES = ROOT / "traces"
REPORTS = ROOT / "reports"

# Baseline L1 for experiments: 8 KB, 64 B blocks, 4-way, LRU, WB+WA.
BASE = {"l1-size": 8192, "l1-block": 64, "l1-assoc": 4, "l1-repl": "lru",
        "l1-write": "back", "l1-alloc": "allocate"}


def run(trace, **overrides):
    """Run one simulation, return the parsed JSON stats object."""
    cfg = dict(BASE)
    cfg.update(overrides)
    cmd = [str(SIM), "--trace", str(trace), "--json"]
    for k, v in cfg.items():
        if v is None:
            cmd.append("--" + k)          # boolean flag (e.g. classify-3c)
        else:
            cmd += ["--" + k, str(v)]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"simulator failed ({' '.join(cmd)}):\n{out.stderr}")
    return json.loads(out.stdout)


def l1(row):
    return row["levels"][0]


def pct(x):
    return f"{100.0 * x:.2f}%"


def mem_writes(row):
    return row["memory"]["writes"]


def main():
    if SIM is None:
        sys.exit("build the simulator first: mingw32-make (or make)")
    for t in ("matmul", "listwalk", "seqscan", "randscan"):
        if not (TRACES / f"{t}.trace").exists():
            sys.exit(f"missing traces/{t}.trace — generate with: mingw32-make traces")
    REPORTS.mkdir(exist_ok=True)

    matmul = TRACES / "matmul.trace"
    csv_rows = []
    md = ["# Comparison report",
          "",
          "Workloads: self-instrumented algorithms over real data-structure addresses",
          "(`make traces`): dense **matmul** (mixed locality), shuffled-list",
          "**listwalk** (pointer chasing), **seqscan** (pure spatial), **randscan**",
          "(no locality). Baseline L1: 8 KB, 64 B blocks, 4-way, LRU, write-back +",
          "write-allocate; AMAT times 1 / 100 cycles (single level unless noted).",
          ""]

    def record(exp, trace, row, extra=None):
        s = l1(row)
        r = {"experiment": exp, "trace": Path(trace).stem, "size": s["size"],
             "block": s["block"], "assoc": s["assoc"], "repl": s["repl"],
             "write": s["write"], "alloc": s["alloc"], "accesses": s["accesses"],
             "misses": s["misses"], "missRate": round(s["missRate"], 6),
             "writebacks": s["writebacks"], "memWrites": mem_writes(row),
             "compulsory": s["compulsory"], "capacity": s["capacity"],
             "conflict": s["conflict"], "amat": round(row["amat"], 4)}
        if extra:
            r.update(extra)
        csv_rows.append(r)
        return s

    # ---- 1. Associativity sweep -----------------------------------------
    md += ["## 1. Associativity sweep (matmul, 8 KB, 64 B blocks)", "",
           "| assoc | miss rate | conflict misses | AMAT |", "|---|---|---|---|"]
    assoc_rows = []
    for a in ("1", "2", "4", "8", "full"):
        row = run(matmul, **{"l1-assoc": a, "classify-3c": None})
        s = record("assoc", matmul, row)
        assoc_rows.append((a, s, row["amat"]))
        md.append(f"| {a} | {pct(s['missRate'])} | {s['conflict']} | {row['amat']:.2f} |")
    dm, four = assoc_rows[0][1], assoc_rows[2][1]
    full = assoc_rows[4][1]
    dm_amat, four_amat = assoc_rows[0][2], assoc_rows[2][2]
    amat_drop = 100.0 * (dm_amat - four_amat) / dm_amat
    md += ["",
           f"**Finding:** direct-mapped → 4-way cuts miss rate "
           f"{pct(dm['missRate'])} → {pct(four['missRate'])} and AMAT "
           f"{dm_amat:.2f} → {four_amat:.2f} cycles ({amat_drop:.1f}% faster), all "
           f"of it conflict misses ({dm['conflict']} → {four['conflict']}) — the "
           f"3-C prediction. But note the *shape*: 4-way and 8-way are identical, "
           f"yet fully-associative collapses conflicts to {full['conflict']} "
           f"({pct(full['missRate'])} miss rate). The culprit is matmul's B-column "
           f"walk: its 512 B power-of-two stride maps all 64 column blocks into "
           f"just 4 of the 32 sets — a 16-deep pileup that no practical way count "
           f"drains. Only removing indexing entirely (full associativity) fixes "
           f"it, which is precisely why real designs facing strided workloads use "
           f"hashed/skewed indexing rather than more ways.", ""]

    # ---- 2. Block-size sweep --------------------------------------------
    md += ["## 2. Block-size sweep (8 KB, 4-way; two contrasting workloads)", "",
           "| block | seqscan miss rate | matmul miss rate |", "|---|---|---|"]
    seq_rows, mm_rows = [], []
    for b in (16, 32, 64, 128):
        rs = run(TRACES / "seqscan.trace", **{"l1-block": b})
        rm = run(matmul, **{"l1-block": b})
        ss = record("block", TRACES / "seqscan.trace", rs)
        sm = record("block", matmul, rm)
        seq_rows.append((b, ss["missRate"]))
        mm_rows.append((b, sm["missRate"]))
        md.append(f"| {b} B | {pct(ss['missRate'])} | {pct(sm['missRate'])} |")
    md += ["",
           f"**Finding:** block size is a *spatial-locality* knob, and the two "
           f"workloads prove it in opposite directions. seqscan's miss rate halves "
           f"with every doubling ({pct(seq_rows[0][1])} → {pct(seq_rows[-1][1])}), "
           f"tracking the 8/block law exactly — one miss loads block/8 sequential "
           f"words. matmul barely moves ({pct(mm_rows[0][1])} → "
           f"{pct(mm_rows[-1][1])}): its misses are dominated by the B-column "
           f"stream whose 512 B stride exceeds any tested block, so no block size "
           f"captures it — the wrong knob for a conflict-dominated workload "
           f"(experiment 1 showed the right one). The textbook U-curve's rising "
           f"side (pollution) needs a reuse-rich working set squeezed by oversized "
           f"blocks; neither pure workload manifests it, and knowing *why* is the "
           f"point of classifying misses before turning knobs.", ""]

    # ---- 3. Capacity sweep ----------------------------------------------
    md += ["## 3. Capacity sweep (matmul, 64 B blocks, 4-way)", "",
           "| size | miss rate | capacity misses | AMAT |", "|---|---|---|---|"]
    cap_rows = []
    for size in (2048, 4096, 8192, 16384, 32768, 65536, 131072):
        row = run(matmul, **{"l1-size": size, "classify-3c": None})
        s = record("capacity", matmul, row)
        cap_rows.append((size, s))
        md.append(f"| {size//1024} KB | {pct(s['missRate'])} | {s['capacity']} | {row['amat']:.2f} |")
    cliff = next((sz for sz, s in cap_rows if s["missRate"] < 0.10), None)
    floor = cap_rows[-1][1]
    floor_pct = 100.0 * floor["compulsory"] / floor["accesses"]
    md += ["",
           f"**Finding:** not a smooth curve but a *staircase*, and each tread is "
           f"a working set. Flat at ~{pct(cap_rows[0][1]['missRate'])} from 2–16 KB "
           f"(the strided B stream misses regardless of capacity — only its "
           f"3-C label shifts between capacity and conflict), then a cliff at "
           f"{cliff//1024} KB — the size at which one 32 KB matrix fits — and by "
           f"64 KB the whole ~96 KB working set is effectively resident: miss rate "
           f"{pct(cap_rows[-1][1]['missRate'])} ≈ the compulsory floor "
           f"({floor['compulsory']} first-touches / {floor['accesses']} accesses "
           f"= {floor_pct:.2f}%), and measured capacity misses reach "
           f"{cap_rows[-1][1]['capacity']}. Capacity helps in quanta, not "
           f"gradients — buy enough for the next working set or save the silicon.", ""]

    # ---- 4. Replacement policies ----------------------------------------
    md += ["## 4. Replacement policy (8 KB, 4-way, matmul and listwalk)", "",
           "| trace | LRU | FIFO | Random |", "|---|---|---|---|"]
    repl_result = {}
    for trace in (matmul, TRACES / "listwalk.trace"):
        rates = {}
        for p in ("lru", "fifo", "random"):
            row = run(trace, **{"l1-repl": p})
            s = record("repl", trace, row)
            rates[p] = s["missRate"]
        repl_result[Path(trace).stem] = rates
        md.append(f"| {Path(trace).stem} | {pct(rates['lru'])} | {pct(rates['fifo'])} | {pct(rates['random'])} |")
    mm, lw = repl_result["matmul"], repl_result["listwalk"]
    md += ["",
           f"**Finding:** on matmul, LRU < FIFO < Random ({pct(mm['lru'])} vs "
           f"{pct(mm['fifo'])} vs {pct(mm['random'])}) — the C accumulator row is "
           f"hot, LRU protects it, FIFO evicts it on schedule. listwalk *inverts* "
           f"the order: LRU ({pct(lw['lru'])}) is slightly WORST ({pct(lw['fifo'])} "
           f"FIFO, {pct(lw['random'])} Random). That's the classic cyclic "
           f"pathology: the 64 KB list is one giant cycle through an 8 KB cache, "
           f"so LRU always evicts exactly the block that will return soonest. "
           f"When reuse distance exceeds capacity, recency is anti-information — "
           f"a measured, honest counterexample to 'LRU is always best'.", ""]

    # ---- 5. Write-policy traffic ----------------------------------------
    md += ["## 5. Write-policy traffic (matmul, 8 KB, 4-way)", "",
           "| policy pair | memory writes | writebacks |", "|---|---|---|"]
    wt = run(matmul, **{"l1-write": "through", "l1-alloc": "no-allocate"})
    wb = run(matmul, **{"l1-write": "back", "l1-alloc": "allocate"})
    s_wt = record("write", matmul, wt, {"write": "through", "alloc": "no-allocate"})
    s_wb = record("write", matmul, wb)
    md.append(f"| write-through + no-allocate | {mem_writes(wt)} | {s_wt['writebacks']} |")
    md.append(f"| write-back + allocate | {mem_writes(wb)} | {s_wb['writebacks']} |")
    ratio = mem_writes(wt) / max(1, mem_writes(wb))
    md += ["",
           f"**Finding:** write-through sends every one of matmul's "
           f"{s_wt['writes']} stores to memory ({mem_writes(wt)} writes); "
           f"write-back coalesces repeated stores to the hot C row into "
           f"{mem_writes(wb)} dirty-eviction write-backs — **{ratio:.0f}× less "
           f"write traffic**. This is the dirty bit earning its keep.", ""]

    # ---- 6. Locality contrast -------------------------------------------
    md += ["## 6. Locality contrast (identical cache: 8 KB, 64 B, 4-way, LRU)", "",
           "| workload | hit rate | AMAT | character |", "|---|---|---|---|"]
    character = {"matmul": "mixed spatial+temporal", "listwalk": "pointer chasing",
                 "seqscan": "pure sequential", "randscan": "uniform random"}
    loc = {}
    for t in ("matmul", "seqscan", "listwalk", "randscan"):
        row = run(TRACES / f"{t}.trace")
        s = record("locality", TRACES / f"{t}.trace", row)
        loc[t] = (s["hitRate"], row["amat"])
        md.append(f"| {t} | {pct(s['hitRate'])} | {row['amat']:.2f} | {character[t]} |")
    md += ["",
           f"**Finding:** the same silicon spans {pct(loc['randscan'][0])} to "
           f"{pct(loc['seqscan'][0])} hit rate purely on access pattern. seqscan "
           f"hits 7 of every 8 accesses mechanically (64 B block = 8 sequential "
           f"words per fill); listwalk defeats spatial locality by design and "
           f"lives at memory speed. Hardware doesn't make code fast — locality "
           f"does; the cache only rewards it.", ""]

    # ---- write outputs ---------------------------------------------------
    csv_path = REPORTS / "comparison.csv"
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(csv_rows[0].keys()))
        w.writeheader()
        w.writerows(csv_rows)

    md_path = REPORTS / "comparison.md"
    md_path.write_text("\n".join(md), encoding="utf-8")

    print(f"wrote {csv_path} ({len(csv_rows)} runs) and {md_path}")


if __name__ == "__main__":
    main()
