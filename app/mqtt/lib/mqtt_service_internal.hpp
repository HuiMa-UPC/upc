#ifndef UPC_MQTT_SERVICE_INTERNAL_HPP
#define UPC_MQTT_SERVICE_INTERNAL_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace upc::mqtt {

inline constexpr uint64_t kReconnectIntervalMs = 3000;
inline constexpr uint64_t kTelemetryPrintingIntervalMs = 2000;
inline constexpr uint64_t kTelemetryIdleIntervalMs = 30000;
inline constexpr uint64_t kFirmwareQueryIntervalMs = 10000;

inline constexpr const char* kCmdSetLight = "set_light";
inline constexpr const char* kCmdStartRtspPush = "start_rtsp_push";
inline constexpr const char* kCmdStopRtspPush = "stop_rtsp_push";
inline constexpr const char* kCmdRejectReasonInvalidState = "invalid_state";
inline constexpr const char* kCmdRejectReasonEncoderError = "encoder_error";
inline constexpr uint64_t kRtspAckTimeoutMs = 8000;
inline constexpr const char* kImageTmpDir = "/home/image/tmp";
inline constexpr const char* kImageSocDir = "/home/image/soc";
inline constexpr const char* kImageMcuDir = "/home/image/mcu";
inline constexpr const char* kImageGlavoDir = "/home/image/glavo";

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string path;
    uint16_t port = 0;
};

void debug_log_mqtt_frame(const char* direction,
                          const std::string& topic,
                          const void* payload,
                          size_t payload_len,
                          int qos,
                          int retained);
bool parse_url(const std::string& raw, ParsedUrl& out);
std::string basename_from_url_path(const std::string& path);
bool ensure_dir(const std::filesystem::path& p);
std::string hex_sha256_file(const std::filesystem::path& p);
bool download_file_with_progress(const std::string& url,
                                 const std::filesystem::path& out_file,
                                 const std::function<void(size_t, size_t)>& on_progress,
                                 std::string& err);
const char* component_target_dir(const std::string& component);
std::string sh_single_quote(const std::string& v);
std::string trim_line(std::string s);
bool archive_entry_safe_flat(const std::string& entry);
bool run_capture_lines(const std::string& cmd, std::vector<std::string>& lines);
bool run_shell_ok(const std::string& cmd);
std::string to_upper_ascii(std::string s);
bool is_bin_name(const std::string& name);
uint64_t activation_retry_interval_ms(uint32_t fail_count);

} // namespace upc::mqtt

#endif
