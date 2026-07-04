#include "memory.h"

bool Memory::access(uint64_t, bool isWrite) {
    if (isWrite) writes_++; else reads_++;
    return true;   // memory always has the data
}

void Memory::report(std::ostream& os) const {
    os << "Memory: reads=" << reads_ << " writes=" << writes_ << "\n";
}
