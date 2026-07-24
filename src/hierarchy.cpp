#include "hierarchy.h"

#include <iomanip>
#include <stdexcept>

CacheHierarchy::CacheHierarchy(const std::vector<CacheConfig>& cfgs, double memTime)
    : memTime_(memTime) {
    if (cfgs.empty())
        throw std::invalid_argument("hierarchy needs at least one cache level");

    // Build deepest-first: a Cache needs its next level at construction time,
    // so Memory is wired first, then L_n, ..., up to L1.
    levels_.resize(cfgs.size());
    MemoryLevel* below = &mem_;
    for (size_t i = cfgs.size(); i-- > 0; ) {
        levels_[i].reset(new Cache(cfgs[i], below));
        below = levels_[i].get();
    }
}

bool CacheHierarchy::access(uint64_t addr, bool isWrite) {
    return levels_.front()->access(addr, isWrite);
}

void CacheHierarchy::one(uint64_t addr, bool isWrite, char opc, std::ostream* vlog) {
    bool hit = access(addr, isWrite);
    if (vlog) {
        Cache::Decoded d = levels_.front()->decode(addr);
        *vlog << "#" << ++fed_ << " " << opc
              << " addr=0x" << std::hex << addr
              << " blk=0x"  << d.blockAddr
              << " set="    << std::dec << d.setIndex
              << " tag=0x"  << std::hex << d.tag << std::dec
              << " -> "     << (hit ? "HIT" : "MISS") << "\n";
    }
}

void CacheHierarchy::feed(const Access& a, std::ostream* vlog) {
    switch (a.op) {
        case Op::Read:   one(a.addr, false, 'R', vlog); break;
        case Op::Write:  one(a.addr, true,  'W', vlog); break;
        case Op::Modify:                       // read-modify-write: two accesses
            one(a.addr, false, 'R', vlog);
            one(a.addr, true,  'W', vlog);
            break;
        case Op::Instr:  break;                // D-cache study: fetches ignored
    }
}

double CacheHierarchy::computeAMAT() const {
    // Miss penalty of level i == AMAT of everything below it, so fold from
    // memory upward: start at memTime, wrap each level around it.
    double penalty = memTime_;
    for (size_t i = levels_.size(); i-- > 0; ) {
        const Stats& s = levels_[i]->stats();
        penalty = levels_[i]->config().hitTime + s.missRate() * penalty;
    }
    return penalty;   // what the CPU sees at L1
}

void CacheHierarchy::reportAll(std::ostream& os) const {
    // Global miss rate divides by ALL CPU accesses (= L1 accesses), not by
    // the accesses that reached the level — that is the local rate, printed
    // by each cache itself.
    uint64_t total = levels_.front()->stats().accesses;

    std::ios_base::fmtflags f = os.flags();
    os << std::fixed << std::setprecision(2);
    for (const auto& c : levels_) {
        c->report(os);
        double global = total ? double(c->stats().misses) / double(total) : 0.0;
        os << "  global missRate=" << global * 100.0
           << "% (misses / " << total << " CPU accesses)\n";
    }
    mem_.report(os);
    os << "AMAT = " << computeAMAT() << " cycles (mem=" << memTime_;
    for (const auto& c : levels_)
        os << ", " << c->config().name << "-hit=" << c->config().hitTime;
    os << ")\n";
    os.flags(f);
}

namespace {

// Minimal JSON string escaping (backslashes in Windows paths, quotes).
std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

const char* replStr(ReplacementType t) {
    switch (t) {
        case ReplacementType::LRU:    return "lru";
        case ReplacementType::FIFO:   return "fifo";
        case ReplacementType::Random: return "random";
    }
    return "?";
}

}  // namespace

void CacheHierarchy::reportJson(std::ostream& os, const std::string& tracePath) const {
    uint64_t total = levels_.front()->stats().accesses;

    std::ios_base::fmtflags f = os.flags();
    os.precision(10);
    os << "{\"trace\":\"" << jsonEscape(tracePath) << "\",\"levels\":[";
    for (size_t i = 0; i < levels_.size(); ++i) {
        const Cache&       c   = *levels_[i];
        const CacheConfig& cfg = c.config();
        const Stats&       s   = c.stats();
        double global = total ? double(s.misses) / double(total) : 0.0;
        if (i) os << ",";
        os << "{\"name\":\"" << cfg.name << "\""
           << ",\"size\":" << cfg.sizeBytes
           << ",\"block\":" << cfg.blockSize
           << ",\"assoc\":" << cfg.associativity
           << ",\"repl\":\"" << replStr(cfg.replacement) << "\""
           << ",\"write\":\"" << (cfg.writePolicy == WritePolicy::WriteBack ? "back" : "through") << "\""
           << ",\"alloc\":\"" << (cfg.allocPolicy == AllocPolicy::WriteAllocate ? "allocate" : "no-allocate") << "\""
           << ",\"hitTime\":" << cfg.hitTime
           << ",\"classify3c\":" << (cfg.classify3C ? "true" : "false")
           << ",\"accesses\":" << s.accesses
           << ",\"hits\":" << s.hits
           << ",\"misses\":" << s.misses
           << ",\"reads\":" << s.reads
           << ",\"writes\":" << s.writes
           << ",\"writebacks\":" << s.writebacks
           << ",\"hitRate\":" << s.hitRate()
           << ",\"missRate\":" << s.missRate()
           << ",\"globalMissRate\":" << global
           << ",\"compulsory\":" << s.compulsory
           << ",\"capacity\":" << s.capacity
           << ",\"conflict\":" << s.conflict
           << "}";
    }
    os << "],\"memory\":{\"reads\":" << mem_.reads()
       << ",\"writes\":" << mem_.writes() << "}"
       << ",\"memTime\":" << memTime_
       << ",\"amat\":" << computeAMAT()
       << "}\n";
    os.flags(f);
}

bool CacheHierarchy::checkInvariants(std::ostream& os) const {
    bool ok = true;
    for (const auto& c : levels_)
        if (!c->checkInvariants(os)) ok = false;
    return ok;
}
