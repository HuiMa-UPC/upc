#include "xfr_config.hpp"
#include "config/cal_config.hpp"

#include <cstdlib>

namespace upc::xfr {

namespace {

const char* env_or_default(const char* key, const char* fallback)
{
    const char* v = std::getenv(key);
    return (v && v[0]) ? v : fallback;
}

bool env_bool(const char* key, bool fallback)
{
    const char* v = std::getenv(key);
    if (!v || !v[0]) {
        return fallback;
    }
    return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' ||
             v[0] == 'f' || v[0] == 'F');
}

uint32_t env_u32(const char* key, uint32_t fallback)
{
    const char* v = std::getenv(key);
    if (!v || !v[0]) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(v, &end, 10);
    if (end == v) {
        return fallback;
    }
    return static_cast<uint32_t>(parsed);
}

} // namespace

XfrConfig load_xfr_config()
{
    XfrConfig cfg;
    cfg.http_port = static_cast<uint16_t>(env_u32("UPC_XFR_PORT", cfg.http_port));
    cfg.storage_dir = env_or_default("UPC_XFR_STORAGE_DIR", cfg.storage_dir.c_str());
    cfg.gcode_dir = env_or_default("UPC_XFR_GCODE_DIR", cfg.gcode_dir.c_str());
    cfg.token_file = env_or_default("UPC_XFR_TOKEN_FILE", cfg.token_file.c_str());
    cfg.mdns_service_file = env_or_default("UPC_XFR_MDNS_SERVICE_FILE",
                                           cfg.mdns_service_file.c_str());
    cfg.snowslice_bin = env_or_default("UPC_XFR_SNOWSLICE_BIN", cfg.snowslice_bin.c_str());
    cfg.progress_file = env_or_default("UPC_XFR_PROGRESS_FILE", cfg.progress_file.c_str());
    cfg.device_sn = env_or_default("UPC_DEVICE_SN", cfg.device_sn.c_str());
    cfg.device_model = env_or_default("UPC_DEVICE_MODEL", cfg.device_model.c_str());
    cfg.require_auth = env_bool("UPC_XFR_REQUIRE_AUTH", cfg.require_auth);
    cfg.chunk_max_bytes = env_u32("UPC_XFR_CHUNK_MAX", cfg.chunk_max_bytes);
    cfg.pair_pin_ttl_s = env_u32("UPC_XFR_PIN_TTL_S", cfg.pair_pin_ttl_s);
    cfg.pair_pin_max_attempts = env_u32("UPC_XFR_PIN_MAX_ATTEMPTS",
                                        cfg.pair_pin_max_attempts);
    cfg.build_timeout_s = env_u32("UPC_XFR_BUILD_TIMEOUT_S", cfg.build_timeout_s);

    upc::config::CalConfigOptions cal_cfg{};
    if(upc::config::load_cal_config_options(upc::config::kCalDefaultPath, &cal_cfg) &&
       cal_cfg.has_config_section) {
        if(cal_cfg.has_device_cn && !cal_cfg.device_cn.empty()) {
            cfg.device_sn = cal_cfg.device_cn;
        }
        if(cal_cfg.has_device_model && !cal_cfg.device_model.empty()) {
            cfg.device_model = cal_cfg.device_model;
        }
    }
    return cfg;
}

} // namespace upc::xfr
