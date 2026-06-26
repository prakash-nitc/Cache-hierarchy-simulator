// Cache Hierarchy Simulator — entry point.
//
// Phase 0 (scaffold): parse CLI arguments, open a trace, and echo every parsed
// (op, address, size) record plus a summary. The cache model itself arrives in
// later phases; this proves the trace front-end works end to end.

#include "trace_reader.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void usage(const char* prog) {
    std::cout
        << "Cache Hierarchy Simulator (phase 0: trace echo)\n"
        << "Usage: " << prog << " --trace <file> [--limit N]\n"
        << "  --trace <file>   path to a memory-access trace (lackey or R/W form)\n"
        << "  --limit N        echo at most N records (0 = unlimited, the default)\n"
        << "  --help, -h       show this message\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string tracePath;
    uint64_t    limit = 0;   // 0 == unlimited

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // Pull the value that must follow a flag, or exit with a clear error.
        auto needVal = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--trace") {
            tracePath = needVal("--trace");
        } else if (arg == "--limit") {
            limit = std::stoull(needVal("--limit"));
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (tracePath.empty()) {
        std::cerr << "error: --trace <file> is required\n";
        usage(argv[0]);
        return 2;
    }

    TraceReader reader(tracePath);
    if (!reader.ok()) {
        std::cerr << "error: cannot open trace file '" << tracePath << "'\n";
        return 1;
    }

    Access   a;
    uint64_t n = 0, reads = 0, writes = 0, modifies = 0, instrs = 0;
    while (reader.next(a)) {
        if (limit == 0 || n < limit) {
            std::cout << "op=" << opName(a.op)
                      << " addr=0x" << std::hex << a.addr << std::dec
                      << " size=" << a.size << "\n";
        }
        switch (a.op) {
            case Op::Read:   ++reads;    break;
            case Op::Write:  ++writes;   break;
            case Op::Modify: ++modifies; break;
            case Op::Instr:  ++instrs;   break;
        }
        ++n;
    }

    std::cout << "---\n"
              << "parsed " << n << " records: "
              << reads << " read, " << writes << " write, "
              << modifies << " modify, " << instrs << " instr\n";
    return 0;
}
