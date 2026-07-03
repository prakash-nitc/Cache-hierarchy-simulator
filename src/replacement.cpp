#include "replacement.h"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

namespace {

// ----- LRU via a monotonically increasing "use clock" -------------------
//
// Every touch (hit or fill) stamps the frame with a global counter. The
// least-recently-used frame is simply the one with the smallest stamp.
// A uint64_t clock cannot realistically overflow (~1.8e19 accesses).
class LRUPolicy : public ReplacementPolicy {
    std::vector<std::vector<uint64_t>> lastUsed;   // [set][way] = clock at last use
    uint64_t clock = 0;
public:
    LRUPolicy(size_t s, size_t a) : lastUsed(s, std::vector<uint64_t>(a, 0)) {}
    void onAccess(size_t set, size_t way) override { lastUsed[set][way] = ++clock; }
    void onInsert(size_t set, size_t way) override { lastUsed[set][way] = ++clock; }
    size_t getVictim(size_t set) override {         // LRU = smallest timestamp
        size_t v = 0;
        uint64_t best = std::numeric_limits<uint64_t>::max();
        for (size_t w = 0; w < lastUsed[set].size(); ++w)
            if (lastUsed[set][w] < best) { best = lastUsed[set][w]; v = w; }
        return v;
    }
};

// ----- FIFO via insertion time only --------------------------------------
//
// Identical machinery to LRU with ONE difference: onAccess is empty. A hit
// does not reorder the queue — eviction follows pure insertion order. That
// single empty method is why LRU usually beats FIFO (it keeps re-used lines)
// and why FIFO can exhibit Belady's anomaly.
class FIFOPolicy : public ReplacementPolicy {
    std::vector<std::vector<uint64_t>> insertTime;  // [set][way] = clock at fill
    uint64_t clock = 0;
public:
    FIFOPolicy(size_t s, size_t a) : insertTime(s, std::vector<uint64_t>(a, 0)) {}
    void onAccess(size_t, size_t) override {}       // re-access does NOT reorder
    void onInsert(size_t set, size_t way) override { insertTime[set][way] = ++clock; }
    size_t getVictim(size_t set) override {         // oldest insertion
        size_t v = 0;
        uint64_t best = std::numeric_limits<uint64_t>::max();
        for (size_t w = 0; w < insertTime[set].size(); ++w)
            if (insertTime[set][w] < best) { best = insertTime[set][w]; v = w; }
        return v;
    }
};

// ----- Random -------------------------------------------------------------
//
// Keeps no state at all; evicts a uniformly random way. Surprisingly decent
// in practice and the cheapest to build in hardware. std::rand() is used
// unseeded, so runs are reproducible on a given platform.
class RandomPolicy : public ReplacementPolicy {
    size_t assoc;
public:
    RandomPolicy(size_t, size_t a) : assoc(a) {}
    void onAccess(size_t, size_t) override {}
    void onInsert(size_t, size_t) override {}
    size_t getVictim(size_t) override {
        return static_cast<size_t>(std::rand()) % assoc;
    }
};

}  // namespace

const char* replName(ReplacementType t) {
    switch (t) {
        case ReplacementType::LRU:    return "LRU";
        case ReplacementType::FIFO:   return "FIFO";
        case ReplacementType::Random: return "Random";
    }
    return "?";
}

std::unique_ptr<ReplacementPolicy>
makeReplacement(ReplacementType t, size_t numSets, size_t assoc) {
    switch (t) {
        case ReplacementType::LRU:    return std::make_unique<LRUPolicy>(numSets, assoc);
        case ReplacementType::FIFO:   return std::make_unique<FIFOPolicy>(numSets, assoc);
        case ReplacementType::Random: return std::make_unique<RandomPolicy>(numSets, assoc);
    }
    return std::make_unique<LRUPolicy>(numSets, assoc);   // unreachable; keeps GCC quiet
}
