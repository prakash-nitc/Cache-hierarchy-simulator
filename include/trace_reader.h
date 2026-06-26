#pragma once
// TraceReader — streams a memory-access trace one record at a time.
//
// Two input formats are accepted:
//   * Valgrind/CMU lackey reduced form:   "I 0400d7d4,8" / " L 7f0004d8,8"
//                                          " S ...,8" / " M ...,4"
//   * Simple fallback:                     "R <hexaddr>" / "W <hexaddr>"
//
// The reader never loads the whole file into memory — large traces can be
// hundreds of millions of lines, so we read line by line.

#include <cstdint>
#include <string>
#include <fstream>

// One memory operation. L/S/M come from the trace; Modify = load THEN store.
enum class Op { Read, Write, Modify, Instr };   // L, S, M, I

struct Access {
    Op       op;     // what kind of operation
    uint64_t addr;   // byte address (parsed from hex)
    uint32_t size;   // access size in bytes (defaults to 1 if absent)
};

// Single-character label for echoing / debugging: R, W, M, I.
const char* opName(Op op);

class TraceReader {
    std::ifstream in_;
public:
    explicit TraceReader(const std::string& path) : in_(path) {}

    // True once the file was opened successfully.
    bool ok() const { return in_.is_open(); }

    // Fill `out` with the next record. Returns false at end of file.
    // Blank lines and malformed lines are skipped defensively.
    bool next(Access& out);
};
