#include "cache.h"

#include <iomanip>
#include <stdexcept>

namespace {

// log2 of a power of two (number of low bits it occupies).
uint64_t log2u(uint64_t x) {
    uint64_t r = 0;
    while (x > 1) { x >>= 1; ++r; }
    return r;
}

bool isPow2(uint64_t x) { return x != 0 && (x & (x - 1)) == 0; }

}  // namespace

Cache::Cache(const CacheConfig& cfg) : cfg_(cfg) {
    if (cfg_.blockSize == 0 || cfg_.associativity == 0 || cfg_.sizeBytes == 0)
        throw std::invalid_argument("cache size/block/associativity must be > 0");
    if (!isPow2(cfg_.blockSize))
        throw std::invalid_argument("blockSize must be a power of two");

    // The geometry identity: numSets = size / (block * associativity).
    if (cfg_.sizeBytes % (cfg_.blockSize * cfg_.associativity) != 0)
        throw std::invalid_argument("size must be a multiple of block * associativity");
    numSets_ = cfg_.sizeBytes / (cfg_.blockSize * cfg_.associativity);
    if (!isPow2(numSets_))
        throw std::invalid_argument("numSets must be a power of two");

    offsetBits_ = log2u(cfg_.blockSize);
    indexBits_  = log2u(numSets_);              // 0 when numSets == 1 (fully-assoc)
    tagBits_    = cfg_.addrWidth - indexBits_ - offsetBits_;

    // Allocate every frame up front; nothing allocates on the hot path.
    sets_.resize(numSets_);
    for (CacheSet& s : sets_) s.lines.resize(cfg_.associativity);

    repl_ = makeReplacement(cfg_.replacement, numSets_, cfg_.associativity);
}

Cache::Decoded Cache::decode(uint64_t addr) const {
    // All fields come from masks/shifts on powers of two — never % or /.
    Decoded d;
    d.offset    = addr & (cfg_.blockSize - 1);
    d.blockAddr = addr >> offsetBits_;
    d.setIndex  = d.blockAddr & (numSets_ - 1);   // low indexBits of the block address
    d.tag       = d.blockAddr >> indexBits_;
    return d;
}

bool Cache::access(uint64_t addr, bool isWrite) {
    stats_.accesses++;
    if (isWrite) stats_.writes++; else stats_.reads++;

    Decoded   d   = decode(addr);
    CacheSet& set = sets_[d.setIndex];

    // ---- search the set for a matching, valid frame (a hit) ----
    for (size_t w = 0; w < set.lines.size(); ++w) {
        CacheLine& line = set.lines[w];
        if (line.valid && line.tag == d.tag) {
            stats_.hits++;
            repl_->onAccess(d.setIndex, w);      // tell the policy this way was re-used
            return true;
        }
    }

    // ---- miss: install the block, evicting if the set is full ----
    stats_.misses++;
    size_t way = pickWay(set, d.setIndex);
    CacheLine& frame = set.lines[way];
    // (Phase 3 adds the dirty-victim write-back here, before overwriting.)
    frame.valid = true;
    frame.tag   = d.tag;
    repl_->onInsert(d.setIndex, way);
    return false;
}

size_t Cache::pickWay(const CacheSet& set, uint64_t setIndex) {
    for (size_t w = 0; w < set.lines.size(); ++w)    // prefer an empty frame
        if (!set.lines[w].valid) return w;
    return repl_->getVictim(static_cast<size_t>(setIndex));  // set full → ask the policy
}

void Cache::report(std::ostream& os) const {
    const char* org = (cfg_.associativity == 1) ? "direct-mapped"
                    : (numSets_ == 1)            ? "fully-associative"
                                                 : "set-associative";
    os << cfg_.name << " (" << org
       << ", size=" << cfg_.sizeBytes << "B"
       << ", block=" << cfg_.blockSize << "B"
       << ", sets="  << numSets_
       << ", assoc=" << cfg_.associativity
       << ", repl="  << replName(cfg_.replacement) << ")\n";
    os << "  bits: offset=" << offsetBits_
       << " index=" << indexBits_
       << " tag="   << tagBits_ << "\n";

    std::ios_base::fmtflags f = os.flags();
    os << std::fixed << std::setprecision(2);
    os << "  accesses=" << stats_.accesses
       << " hits="   << stats_.hits
       << " misses=" << stats_.misses
       << "  hitRate="  << stats_.hitRate()  * 100.0 << "%"
       << " missRate=" << stats_.missRate() * 100.0 << "%\n";
    os.flags(f);
}
