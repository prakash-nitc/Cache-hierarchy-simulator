// Cache Hierarchy Simulator — entry point.
//
// Phase 2: build one read-only cache of any associativity (direct-mapped to
// fully-associative) with a pluggable replacement policy, run a trace through
// it, and report hit/miss stats. (Writes, hierarchy and the 3 C's arrive in
// later phases.)

#include "cache.h"
#include "config.h"
#include "memory.h"
#include "trace_reader.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void usage(const char* prog) {
    std::cout
        << "Cache Hierarchy Simulator (phase 2: associativity + replacement, read-only)\n"
        << "Usage: " << prog << " --trace <file> --l1-size <bytes> --l1-block <bytes>\n"
        << "               [--l1-assoc N|full] [--l1-repl lru|fifo|random]\n"
        << "               [--addr-bits N] [--verbose]\n"
        << "  --trace <file>     memory-access trace (lackey or R/W form)   [required]\n"
        << "  --l1-size <bytes>  total cache capacity in bytes              [required]\n"
        << "  --l1-block <bytes> block/line size in bytes (power of two)    [required]\n"
        << "  --l1-assoc N|full  ways per set: 1 = direct-mapped (default),\n"
        << "                     'full' = fully-associative (assoc = size/block)\n"
        << "  --l1-repl P        replacement policy: lru (default), fifo, random\n"
        << "  --addr-bits N      address width in bits (default 64)\n"
        << "  --verbose          print the decode + HIT/MISS for each access\n"
        << "  --help, -h         show this message\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string tracePath;
    CacheConfig cfg;            // name="L1", assoc=1, repl=LRU, addrWidth=64 by default
    std::string assocStr;       // may be "full"; resolved once size/block are known
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
        } else if (arg == "--l1-assoc") {
            assocStr = needVal("--l1-assoc");
        } else if (arg == "--l1-repl") {
            std::string p = needVal("--l1-repl");
            if      (p == "lru")    cfg.replacement = ReplacementType::LRU;
            else if (p == "fifo")   cfg.replacement = ReplacementType::FIFO;
            else if (p == "random") cfg.replacement = ReplacementType::Random;
            else {
                std::cerr << "error: --l1-repl must be lru, fifo or random (got '"
                          << p << "')\n";
                return 2;
            }
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

    // Resolve associativity now that size/block are known. "full" means one
    // set holding every line: assoc = totalLines = size / block.
    if (assocStr == "full") {
        cfg.associativity = cfg.sizeBytes / cfg.blockSize;
    } else if (!assocStr.empty()) {
        cfg.associativity = std::stoull(assocStr);
    }

    TraceReader reader(tracePath);
    if (!reader.ok()) {
        std::cerr << "error: cannot open trace file '" << tracePath << "'\n";
        return 1;
    }

    // The terminal level: stores forwarded by write-through, write-around and
    // dirty write-backs all land here and are counted.
    Memory mem;

    // Construct up front so an impossible geometry (non-power-of-two block,
    // size not divisible by block*assoc, ...) fails with a clear message.
    std::unique_ptr<Cache> cache;
    try {
        cache.reset(new Cache(cfg, &mem));
    } catch (const std::exception& e) {
        std::cerr << "error: bad cache geometry: " << e.what() << "\n";
        return 2;
    }

    Access   a;
    uint64_t n = 0;
    while (reader.next(a)) {
        // Instruction fetches are ignored in this data-cache study. L is a
        // read, S a write. M (load-then-store) is still modeled as its load
        // half only — the expansion into two accesses is Phase 4's feed().
        if (a.op == Op::Instr) continue;

        bool isWrite = (a.op == Op::Write);
        bool hit = cache->access(a.addr, isWrite);

        if (verbose) {
            Cache::Decoded d = cache->decode(a.addr);
            std::cout << "#" << ++n
                      << " addr=0x" << std::hex << a.addr
                      << " blk=0x"  << d.blockAddr
                      << " set="    << std::dec << d.setIndex
                      << " tag=0x"  << std::hex << d.tag << std::dec
                      << " -> "     << (hit ? "HIT" : "MISS") << "\n";
        }
    }

    cache->report(std::cout);
    mem.report(std::cout);
    if (!cache->checkInvariants(std::cerr)) {
        std::cerr << "error: invariants violated — simulation results are unreliable\n";
        return 1;
    }
    std::cout << "invariants: OK\n";
    return 0;
}
