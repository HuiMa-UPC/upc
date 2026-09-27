#include "n2_ble_parse.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace upc::glavo {
namespace {

std::string trim_copy(std::string_view in)
{
    size_t b = 0;
    size_t e = in.size();
    while (b < e && std::isspace(static_cast<unsigned char>(in[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1]))) {
        --e;
    }
    return std::string(in.substr(b, e - b));
}

N2BleState parse_state_token(std::string_view tok)
{
    if (tok == "WARMUP") {
        return N2BleState::Warmup;
    }
    if (tok == "BUILD") {
        return N2BleState::Build;
    }
    if (tok == "RUN") {
        return N2BleState::Run;
    }
    if (tok == "SAFE") {
        return N2BleState::Safe;
    }
    return N2BleState::Unknown;
}

} // namespace

const char* n2_ble_state_name(N2BleState st)
{
    switch (st) {
    case N2BleState::Warmup: return "WARMUP";
    case N2BleState::Build:  return "BUILD";
    case N2BleState::Run:    return "RUN";
    case N2BleState::Safe:   return "SAFE";
    case N2BleState::Unknown:
    default:                 return "UNKNOWN";
    }
}

bool parse_n2_ble_ascii(std::string_view text, N2BleParsed* out)
{
    if (!out) {
        return false;
    }
    *out = N2BleParsed{};
    const std::string line = trim_copy(text);
    if (line.empty()) {
        return false;
    }

    bool saw_n2 = false;
    size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        if (pos >= line.size()) {
            break;
        }
        const size_t start = pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        const std::string tok = line.substr(start, pos - start);
        if (tok.compare(0, 3, "N2=") == 0) {
            saw_n2 = true;
            const std::string val = tok.substr(3);
            if (val == "warming" || val == "waiting") {
                out->n2_valid = false;
                out->n2_percent = 0.f;
            } else {
                char* end = nullptr;
                const float pct = std::strtof(val.c_str(), &end);
                if (end == val.c_str()) {
                    return false;
                }
                out->n2_valid = true;
                out->n2_percent = pct;
            }
        } else if (tok.compare(0, 6, "STATE=") == 0) {
            out->has_state = true;
            out->state = parse_state_token(tok.substr(6));
        } else if (tok.compare(0, 4, "seq=") == 0) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(tok.c_str() + 4, &end, 10);
            if (end != tok.c_str() + 4) {
                out->has_seq = true;
                out->seq = static_cast<uint32_t>(v);
            }
        }
    }
    return saw_n2;
}

} // namespace upc::glavo
