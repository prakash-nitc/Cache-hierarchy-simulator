// Cache Hierarchy Simulator — entry point.
//
// Phase 4: a real hierarchy. The CLI builds L1 (and optionally L2), chains
// them over Memory inside CacheHierarchy, expands M ops into load+store,
// and reports per-level local/global miss rates plus AMAT.

#include "config.h"
#include "hierarchy.h"
#include "trace_reader.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void usage(const char* prog) {
    std::cout
        << "Cache Hierarchy Simulator (phase 4: L1 -> L2 -> memory + AMAT)\n"
        << "Usage: " << prog << " --trace <file> --l1-size <bytes> --l1-block <bytes>\n"
        << "               [--l1-assoc N|full] [--l1-repl lru|fifo|random]\n"
        << "               [--l1-write back|through] [--l1-alloc allocate|no-allocate]\n"
        << "               [--l2-size <bytes> --l2-block <bytes>] [--l2-assoc N|full]\n"
        << "               [--l2-repl P] [--l2-write W] [--l2-alloc A]\n"
        << "               [--l1-hit C] [--l2-hit C] [--mem-time C]\n"
        << "               [--addr-bits N] [--verbose]\n"
        << "  --trace <file>      memory-access trace (lackey or R/W form)  [required]\n"
        << "  --l1-size/-block    L1 capacity / block size in bytes         [required]\n"
        << "  --l1-assoc N|full   L1 ways per set (default 1 = direct-mapped)\n"
        << "  --l1-repl P         lru (default) | fifo | random\n"
        << "  --l1-write W        back (default) | through\n"
        << "  --l1-alloc A        allocate (default) | no-allocate\n"
        << "  --l2-size/-block    add an L2 with this capacity / block size\n"
        << "  --l2-assoc/repl/... L2 knobs, same values as the L1 forms\n"
        << "  --l1-hit C          L1 hit time in cycles   (default 1)\n"
        << "  --l2-hit C          L2 hit time in cycles   (default 10)\n"
        << "  --mem-time C        memory access in cycles (default 100)\n"
        << "  --addr-bits N       address width in bits (default 64)\n"
        << "  --verbose           echo every CPU access with its L1 decode\n"
        << "  --help, -h          show this message\n";
}

bool parseRepl(const std::string& v, ReplacementType& out) {
    if (v == "lru")    { out = ReplacementType::LRU;    return true; }
    if (v == "fifo")   { out = ReplacementType::FIFO;   return true; }
    if (v == "random") { out = ReplacementType::Random; return true; }
    return false;
}

bool parseWrite(const std::string& v, WritePolicy& out) {
    if (v == "back")    { out = WritePolicy::WriteBack;    return true; }
    if (v == "through") { out = WritePolicy::WriteThrough; return true; }
    return false;
}

bool parseAlloc(const std::string& v, AllocPolicy& out) {
    if (v == "allocate")    { out = AllocPolicy::WriteAllocate;   return true; }
    if (v == "no-allocate") { out = AllocPolicy::NoWriteAllocate; return true; }
    return false;
}

// Resolve an associativity argument ("4" or "full") once geometry is known.
uint64_t resolveAssoc(const std::string& s, const CacheConfig& cfg) {
    if (s == "full") return cfg.sizeBytes / cfg.blockSize;
    return std::stoull(s);
}

}  // namespace

int main(int argc, char** argv) {
    std::string tracePath;
    CacheConfig l1;  l1.name = "L1";  l1.hitTime = 1.0;
    CacheConfig l2;  l2.name = "L2";  l2.hitTime = 10.0;
    std::string l1AssocStr, l2AssocStr;
    double      memTime   = 100.0;
    bool        haveL1Size = false, haveL1Block = false;
    bool        haveL2Size = false, haveL2Block = false;
    bool        anyL2      = false;   // any --l2-* flag seen
    bool        verbose    = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto needVal = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "error: " << name << " requires a value\n";
                std::exit(2);
            }
            return argv[++i];
        };
        auto badValue = [&](const char* flag, const std::string& got,
                            const char* valid) -> int {
            std::cerr << "error: " << flag << " must be " << valid
                      << " (got '" << got << "')\n";
            return 2;
        };

        if      (arg == "--trace")     { tracePath = needVal("--trace"); }
        else if (arg == "--l1-size")   { l1.sizeBytes = std::stoull(needVal(arg.c_str())); haveL1Size = true; }
        else if (arg == "--l1-block")  { l1.blockSize = std::stoull(needVal(arg.c_str())); haveL1Block = true; }
        else if (arg == "--l1-assoc")  { l1AssocStr = needVal(arg.c_str()); }
        else if (arg == "--l1-repl")   { std::string v = needVal(arg.c_str());
            if (!parseRepl(v, l1.replacement))  return badValue("--l1-repl", v, "lru, fifo or random"); }
        else if (arg == "--l1-write")  { std::string v = needVal(arg.c_str());
            if (!parseWrite(v, l1.writePolicy)) return badValue("--l1-write", v, "back or through"); }
        else if (arg == "--l1-alloc")  { std::string v = needVal(arg.c_str());
            if (!parseAlloc(v, l1.allocPolicy)) return badValue("--l1-alloc", v, "allocate or no-allocate"); }
        else if (arg == "--l2-size")   { l2.sizeBytes = std::stoull(needVal(arg.c_str())); haveL2Size = true; anyL2 = true; }
        else if (arg == "--l2-block")  { l2.blockSize = std::stoull(needVal(arg.c_str())); haveL2Block = true; anyL2 = true; }
        else if (arg == "--l2-assoc")  { l2AssocStr = needVal(arg.c_str()); anyL2 = true; }
        else if (arg == "--l2-repl")   { std::string v = needVal(arg.c_str()); anyL2 = true;
            if (!parseRepl(v, l2.replacement))  return badValue("--l2-repl", v, "lru, fifo or random"); }
        else if (arg == "--l2-write")  { std::string v = needVal(arg.c_str()); anyL2 = true;
            if (!parseWrite(v, l2.writePolicy)) return badValue("--l2-write", v, "back or through"); }
        else if (arg == "--l2-alloc")  { std::string v = needVal(arg.c_str()); anyL2 = true;
            if (!parseAlloc(v, l2.allocPolicy)) return badValue("--l2-alloc", v, "allocate or no-allocate"); }
        else if (arg == "--l1-hit")    { l1.hitTime = std::stod(needVal(arg.c_str())); }
        else if (arg == "--l2-hit")    { l2.hitTime = std::stod(needVal(arg.c_str())); anyL2 = true; }
        else if (arg == "--mem-time")  { memTime = std::stod(needVal(arg.c_str())); }
        else if (arg == "--addr-bits") { l1.addrWidth = l2.addrWidth = std::stoull(needVal(arg.c_str())); }
        else if (arg == "--verbose")   { verbose = true; }
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        else {
            std::cerr << "error: unknown option '" << arg << "'\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (tracePath.empty() || !haveL1Size || !haveL1Block) {
        std::cerr << "error: --trace, --l1-size and --l1-block are required\n";
        usage(argv[0]);
        return 2;
    }
    if (anyL2 && (!haveL2Size || !haveL2Block)) {
        std::cerr << "error: L2 flags given but --l2-size and --l2-block are required"
                     " to enable an L2\n";
        return 2;
    }

    if (!l1AssocStr.empty()) l1.associativity = resolveAssoc(l1AssocStr, l1);
    if (!l2AssocStr.empty()) l2.associativity = resolveAssoc(l2AssocStr, l2);

    TraceReader reader(tracePath);
    if (!reader.ok()) {
        std::cerr << "error: cannot open trace file '" << tracePath << "'\n";
        return 1;
    }

    std::vector<CacheConfig> cfgs;
    cfgs.push_back(l1);
    if (haveL2Size) cfgs.push_back(l2);

    std::unique_ptr<CacheHierarchy> hier;
    try {
        hier.reset(new CacheHierarchy(cfgs, memTime));
    } catch (const std::exception& e) {
        std::cerr << "error: bad cache geometry: " << e.what() << "\n";
        return 2;
    }

    Access a;
    while (reader.next(a))
        hier->feed(a, verbose ? &std::cout : nullptr);

    hier->reportAll(std::cout);
    if (!hier->checkInvariants(std::cerr)) {
        std::cerr << "error: invariants violated — simulation results are unreliable\n";
        return 1;
    }
    std::cout << "invariants: OK\n";
    return 0;
}
