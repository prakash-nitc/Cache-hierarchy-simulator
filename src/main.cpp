// Cache Hierarchy Simulator — entry point.
//
// Phase 1: build one direct-mapped, read-only cache from the CLI, run a trace
// through it, and report hit/miss stats. (Writes, associativity, hierarchy and
// the 3 C's arrive in later phases.)

#include "cache.h"
#include "config.h"
#include "trace_reader.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void usage(const char* prog) {
    std::cout
        << "Cache Hierarchy Simulator (phase 1: direct-mapped, read-only)\n"
        << "Usage: " << prog << " --trace <file> --l1-size <bytes> --l1-block <bytes>\n"
        << "               [--addr-bits N] [--verbose]\n"
        << "  --trace <file>     memory-access trace (lackey or R/W form)   [required]\n"
        << "  --l1-size <bytes>  total cache capacity in bytes              [required]\n"
        << "  --l1-block <bytes> block/line size in bytes (power of two)    [required]\n"
        << "  --addr-bits N      address width in bits (default 64)\n"
        << "  --verbose          print the decode + HIT/MISS for each access\n"
        << "  --help, -h         show this message\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string tracePath;
    CacheConfig cfg;            // name="L1", assoc=1, addrWidth=64 by default
    bool        haveSize  = false;
    bool        haveBlock = false;
    bool        verbose   = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto needVal = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--trace") {
            tracePath = needVal("--trace");
        } else if (arg == "--l1-size") {
            cfg.sizeBytes = std::stoull(needVal("--l1-size"));
            haveSize = true;
        } else if (arg == "--l1-block") {
            cfg.blockSize = std::stoull(needVal("--l1-block"));
            haveBlock = true;
        } else if (arg == "--addr-bits") {
            cfg.addrWidth = std::stoull(needVal("--addr-bits"));
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (tracePath.empty() || !haveSize || !haveBlock) {
        std::cerr << "error: --trace, --l1-size and --l1-block are required\n";
        usage(argv[0]);
        return 2;
    }

    TraceReader reader(tracePath);
    if (!reader.ok()) {
        std::cerr << "error: cannot open trace file '" << tracePath << "'\n";
        return 1;
    }

    Cache cache(cfg);   // may throw on bad geometry

    Access   a;
    uint64_t n = 0;
    while (reader.next(a)) {
        // Phase 1 is read-only: every data op is modeled as a single lookup.
        // Instruction fetches are ignored in this data-cache study.
        if (a.op == Op::Instr) continue;

        bool hit = cache.access(a.addr, /*isWrite=*/false);

        if (verbose) {
            Cache::Decoded d = cache.decode(a.addr);
            std::cout << "#" << ++n
                      << " addr=0x" << std::hex << a.addr
                      << " blk=0x"  << d.blockAddr
                      << " set="    << std::dec << d.setIndex
                      << " tag=0x"  << std::hex << d.tag << std::dec
                      << " -> "     << (hit ? "HIT" : "MISS") << "\n";
        }
    }

    cache.report(std::cout);
    return 0;
}
