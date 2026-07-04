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

Cache::Cache(const CacheConfig& cfg, MemoryLevel* next) : cfg_(cfg), next_(next) {
    if (next_ == nullptr)
        throw std::invalid_argument("cache requires a next level (memory or cache)");
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
            if (isWrite) {
                if (cfg_.writePolicy == WritePolicy::WriteThrough)
                    next_->access(addr, /*isWrite=*/true);   // forward the store below
                else
                    line.dirty = true;           // write-back: defer until eviction
            }
            return true;
        }
    }

    // ---- miss ----
    stats_.misses++;

    // Write-around: a store that misses under no-write-allocate goes straight
    // below; nothing is cached and no frame changes.
    if (isWrite && cfg_.allocPolicy == AllocPolicy::NoWriteAllocate) {
        next_->access(addr, /*isWrite=*/true);
        return false;
    }

    // Allocate path: read miss, or write miss with write-allocate.
    next_->access(addr, /*isWrite=*/false);      // FETCH the block (a read from below)

    size_t way = pickWay(set, d.setIndex);
    CacheLine& frame = set.lines[way];
    if (frame.valid && frame.dirty && cfg_.writePolicy == WritePolicy::WriteBack) {
        // Evicting a modified line: it is the only copy, so write it back.
        // Rebuild the victim's byte address from its tag + this set's index —
        // when the next level is a cache (Phase 4), the write-back must land
        // in the victim's own set there, not this access's.
        stats_.writebacks++;
        uint64_t vBlock = (frame.tag << indexBits_) | d.setIndex;
        uint64_t vAddr  = vBlock << offsetBits_;
        next_->access(vAddr, /*isWrite=*/true);
    }

    frame.valid = true;                          // install the fetched block
    frame.tag   = d.tag;
    if (isWrite) {
        if (cfg_.writePolicy == WritePolicy::WriteThrough) {
            frame.dirty = false;                 // below is current: line stays clean
            next_->access(addr, /*isWrite=*/true);
        } else {
            frame.dirty = true;                  // write-back: new line starts dirty
        }
    } else {
        frame.dirty = false;                     // a read fill is clean
    }
    repl_->onInsert(d.setIndex, way);
    return false;
}

bool Cache::checkInvariants(std::ostream& os) const {
    bool ok = true;
    if (stats_.hits + stats_.misses != stats_.accesses) {
        os << "INVARIANT VIOLATED: hits(" << stats_.hits << ") + misses("
           << stats_.misses << ") != accesses(" << stats_.accesses << ")\n";
        ok = false;
    }
    for (size_t s = 0; s < sets_.size(); ++s)
        for (size_t w = 0; w < sets_[s].lines.size(); ++w)
            if (sets_[s].lines[w].dirty && !sets_[s].lines[w].valid) {
                os << "INVARIANT VIOLATED: line dirty && !valid at set "
                   << s << " way " << w << "\n";
                ok = false;
            }
    return ok;
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
    const char* wp = (cfg_.writePolicy == WritePolicy::WriteBack) ? "back" : "through";
    const char* ap = (cfg_.allocPolicy == AllocPolicy::WriteAllocate) ? "allocate"
                                                                      : "no-allocate";
    os << cfg_.name << " (" << org
       << ", size=" << cfg_.sizeBytes << "B"
       << ", block=" << cfg_.blockSize << "B"
       << ", sets="  << numSets_
       << ", assoc=" << cfg_.associativity
       << ", repl="  << replName(cfg_.replacement)
       << ", write=" << wp << ", alloc=" << ap << ")\n";
    os << "  bits: offset=" << offsetBits_
       << " index=" << indexBits_
       << " tag="   << tagBits_ << "\n";

    std::ios_base::fmtflags f = os.flags();
    os << std::fixed << std::setprecision(2);
    os << "  accesses=" << stats_.accesses
       << " hits="   << stats_.hits
       << " misses=" << stats_.misses
       << " (reads=" << stats_.reads
       << " writes=" << stats_.writes
       << " writebacks=" << stats_.writebacks << ")"
       << "  hitRate="  << stats_.hitRate()  * 100.0 << "%"
       << " missRate=" << stats_.missRate() * 100.0 << "%\n";
    os.flags(f);
}
