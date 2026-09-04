#ifndef UPC_XFR_CONFIG_HPP
#define UPC_XFR_CONFIG_HPP

#include "gcode_manifest.hpp"
#include "xfr_paths.hpp"

#include <cstdint>
#include <string>

namespace upc::xfr {

struct XfrConfig {
    uint16_t http_port = 8080;
    std::string storage_dir = UPC_XFR_STORAGE_DIR;
    std::string gcode_dir = UPC_LOCAL_GCODE_DIR;
    std::string token_file = UPC_XFR_TOKEN_FILE;
    std::string mdns_service_file = "/etc/avahi/services/snowpod.service";
    std::string snowslice_bin = "/home/snowslice";
    std::string progress_file = "/tmp/progress.txt";
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
