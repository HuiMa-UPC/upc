#ifndef UPC_XFR_CONFIG_HPP
#define UPC_XFR_CONFIG_HPP

#include <cstdint>
#include <string>

namespace upc::xfr {

struct XfrConfig {
    uint16_t http_port = 8080;
    std::string storage_dir = "/home/transport";
    std::string gcode_dir = "/home/Gcode";
    std::string token_file = "/home/transport/.xfr_tokens";
    std::string mdns_service_file = "/etc/avahi/services/snowpod.service";
    std::string device_sn = "SN-test03";
    std::string device_model = "Snowpod";
    bool require_auth = true;
    uint32_t chunk_max_bytes = 4U * 1024U * 1024U;
    uint32_t pair_pin_ttl_s = 120U;
    uint32_t pair_pin_max_attempts = 5U;
};

XfrConfig load_xfr_config();

} // namespace upc::xfr

#endif
