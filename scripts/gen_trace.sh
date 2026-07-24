#!/usr/bin/env bash
# gen_trace.sh — record a real program's memory accesses with Valgrind lackey
# (Linux/macOS; on machines without Valgrind use the tracegen fallback:
#  see the 'traces' target in the Makefile).
#
# Usage:   ./scripts/gen_trace.sh traces/gzip.trace gzip -9 somefile
#          ./scripts/gen_trace.sh <output.trace> <program> [args...]
#
# Keeps only data accesses (L/S/M) — I lines are dropped for a D-cache study.

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <output.trace> <program> [args...]" >&2
    exit 2
fi

out=$1
shift

raw=$(mktemp)
trap 'rm -f "$raw"' EXIT

# lackey writes the trace on stderr; the program's own stdout passes through.
valgrind --tool=lackey --trace-mem=yes "$@" 2> "$raw"

grep -E '^[ ]?[LSM]' "$raw" > "$out"
echo "wrote $out ($(wc -l < "$out") data accesses)" >&2
