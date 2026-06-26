#include "trace_reader.h"

const char* opName(Op op) {
    switch (op) {
        case Op::Read:   return "R";
        case Op::Write:  return "W";
        case Op::Modify: return "M";
        case Op::Instr:  return "I";
    }
    return "?";
}

bool TraceReader::next(Access& out) {
    std::string line;
    while (std::getline(in_, line)) {
        // Windows-authored traces may carry a trailing CR; strip it so the
        // size field parses cleanly.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        Op op;
        size_t p;   // index of the first char AFTER the op character

        if (line[0] == 'I') {            // 'I' in column 0 == instruction fetch
            op = Op::Instr;
            p  = 1;
        } else {
            // Data ops (and the fallback R/W form) are indented by a space.
            size_t i = line.find_first_not_of(" \t");
            if (i == std::string::npos) continue;   // whitespace-only line
            switch (line[i]) {
                case 'L': op = Op::Read;   break;   // load
                case 'S': op = Op::Write;  break;   // store
                case 'M': op = Op::Modify; break;   // modify = load then store
                case 'I': op = Op::Instr;  break;   // indented instruction fetch
                case 'R': op = Op::Read;   break;   // fallback "R <hex>"
                case 'W': op = Op::Write;  break;   // fallback "W <hex>"
                default:  continue;                 // unknown op → skip defensively
            }
            p = i + 1;
        }

        // Remainder looks like "  04ec4af0,4"  (address, optional ",size").
        std::string rest = line.substr(p);
        size_t comma = rest.find(',');
        std::string addrStr = (comma == std::string::npos) ? rest : rest.substr(0, comma);
        std::string sizeStr = (comma == std::string::npos) ? std::string()
                                                           : rest.substr(comma + 1);

        // Trim whitespace around the address token.
        size_t a0 = addrStr.find_first_not_of(" \t");
        if (a0 == std::string::npos) continue;       // no address present
        size_t a1 = addrStr.find_last_not_of(" \t");
        addrStr = addrStr.substr(a0, a1 - a0 + 1);

        try {
            // Addresses are HEX (base 16). Parsing as decimal is the #1 trace bug.
            out.addr = std::stoull(addrStr, nullptr, 16);
            out.size = sizeStr.empty() ? 1u
                                       : static_cast<uint32_t>(std::stoul(sizeStr));
        } catch (...) {
            continue;                                // malformed number → skip
        }
        out.op = op;
        return true;
    }
    return false;   // EOF
}
