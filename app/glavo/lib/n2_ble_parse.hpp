#ifndef UPC_GLAVO_N2_BLE_PARSE_HPP
#define UPC_GLAVO_N2_BLE_PARSE_HPP

/**
 * @file n2_ble_parse.hpp
 * @brief 制氮机 ESP BLE ASCII 解析（与 D-Bus 无关，可单测）。
 *
 * 例：`N2=99.50% STATE=RUN seq=1`
 *     `N2=warming STATE=WARMUP seq=0`
 *     `N2=waiting`
 */

#include <cstdint>
#include <string>
#include <string_view>

namespace upc::glavo {

/** 与 N2StatusPayload::state 数值一致。 */
enum class N2BleState : uint8_t {
    Unknown = 0,
    Warmup  = 1,
    Build   = 2,
    Run     = 3,
    Safe    = 4,
};

struct N2BleParsed {
    bool       n2_valid   = false;
    float      n2_percent = 0.f;
    N2BleState state      = N2BleState::Unknown;
    uint32_t   seq        = 0;
    bool       has_state  = false;
    bool       has_seq    = false;
};

bool parse_n2_ble_ascii(std::string_view text, N2BleParsed* out);

const char* n2_ble_state_name(N2BleState st);

} // namespace upc::glavo

#endif /* UPC_GLAVO_N2_BLE_PARSE_HPP */
