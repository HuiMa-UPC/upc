#ifndef UPC_MQTT_SERVICE_HPP
#define UPC_MQTT_SERVICE_HPP

#include "mqtt_config.hpp"
#include "device_activation.hpp"
#include "upc_contract.hpp"
#include "msg_port.hpp"

#include <MQTTClient.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace upc::mqtt {

class MqttService {
public:
    MqttService();
    ~MqttService();

    MqttService(const MqttService&) = delete;
    MqttService& operator=(const MqttService&) = delete;

    bool init();
    void shutdown();
    void tick();

    /** 云端 log_collect 下行业务体（忽略 timeRange*）。 */
    struct LogCollectCommand {
        std::string req_id;
        std::string collect_id;
        std::string object_key;
        std::string upload_url;
        int64_t upload_url_expires_at = 0; /**< Unix 秒 */
        int64_t max_packet_size = 0;       /**< 密文包上限（含 nonce+tag） */
        std::string encrypt_salt_b64;      /**< 下行原文，回显不重编 */
        std::vector<std::string> topic_files;
    };

    struct LogFileReport {
        std::string file;
        bool collected = false;
        int64_t size = 0;       /**< collected=true 或 empty/size_exceeded 时 */
        std::string reason;     /**< collected=false 时：not_found/empty/size_exceeded */
    };

private:
    struct ChamberTelemetry {
        float pm25 = 0.0f;
        float pm1_0 = -1.0f;
        float pm10 = -1.0f;
        float oxygen_percent = 0.0f;
        float temperature_c = 0.0f;
        float humidity_percent = -1.0f;
        float pressure_kpa = -1.0f;
        float baseplate_temp_c = -1.0f;
        uint32_t status = 0;
        bool wifi_connected = false;
        int signal_bars = -1;
        bool has_value = false;
    };

    struct LightCommand {
        std::string req_id;
        std::string mode;
        int brightness = -1;
    };

    struct RtspCommand {
        std::string req_id;
        std::string stream_type;
        std::string push_url;
        std::string push_token;
        std::string recording_id;
        uint32_t seek_s = 0;
        uint32_t max_bitrate_kbps = 0;
    };

    struct PendingAck {
        std::string cmd;
        uint64_t deadline_ms = 0;
    };

    struct FirmwareCache {
        bool mcu_valid = false;
        bool galvo_valid = false;
        middleware::McuVersionPayload mcu{};
        middleware::GalvoVersionPayload galvo{};
    };

    struct OtaNotify {
        std::string task_id;
        std::string release_version;
        bool required = false;
        middleware::OtaApprovalStatus approval_status =
            middleware::OtaApprovalStatus::Unknown;
    };

    struct OtaComponent {
        std::string component;      /**< soc / mcu / galvo */
        std::string version;
        std::string url;
        std::string sha256;
        uint64_t size_bytes = 0;
        std::string file_name;
    };

    void subscribe_messages();
    void on_env_update(const middleware::Message& msg);
    void on_mcu_version(const middleware::Message& msg);
    void on_mcu_ota_status(const middleware::Message& msg);
    void on_galvo_version(const middleware::Message& msg);
    void on_env_sim_state(const middleware::Message& msg);
    void on_print_state(const middleware::Message& msg);
    void on_ota_user_decision(const middleware::Message& msg);
    void on_rtsp_start_rsp(const middleware::Message& msg);
    void on_rtsp_stop_rsp(const middleware::Message& msg);
    void cache_telemetry(const middleware::EnvSnapshotPayload& p);
    bool read_env_payload(const middleware::Message& msg,
                          middleware::EnvSnapshotPayload& out) const;
    void request_firmware_versions_if_needed();
    void expire_pending_acks();

    bool ensure_mqtt_connected();
    bool connect_mqtt();
    void disconnect_mqtt();
    void try_reconnect();
    bool subscribe_cmd_topic();
    void start_activation();
    void poll_activation();
    void join_activation_thread();

    bool publish_chamber_telemetry();
    bool publish_heartbeat();
    bool publish_state(uint32_t mode, uint64_t since_ms);
    bool publish_cmd_ack(const std::string& req_id,
                         const std::string& cmd,
                         const std::string& result,
                         const char* reason,
                         const char* detail_json = "{}");

    void handle_command_message(const std::string& payload);
    bool parse_common_command(const std::string& payload,
                              std::string& req_id,
                              std::string& cmd,
                              std::string& data_json,
                              std::string& reject_reason) const;
    bool parse_light_command(const std::string& data_json,
                             const std::string& req_id,
                             LightCommand& out,
                             std::string& reject_reason) const;
    bool parse_rtsp_start_command(const std::string& data_json,
                                  const std::string& req_id,
                                  RtspCommand& out,
                                  std::string& reject_reason) const;
    bool parse_rtsp_stop_command(const std::string& data_json,
                                 const std::string& req_id,
                                 RtspCommand& out,
                                 std::string& reject_reason) const;
    bool parse_log_collect_command(const std::string& data_json,
                                   const std::string& req_id,
                                   LogCollectCommand& out,
                                   std::string& reject_reason) const;
    bool dispatch_set_light_to_glavo(const LightCommand& cmd);
    bool dispatch_rtsp_start_to_media(const RtspCommand& cmd);
    bool dispatch_rtsp_stop_to_media(const RtspCommand& cmd);
    void handle_rtsp_rsp(const middleware::Message& msg, const char* expect_cmd);
    bool track_pending_ack(const std::string& req_id, const std::string& cmd);
    bool start_log_collect(LogCollectCommand cmd);
    void run_log_collect_task(LogCollectCommand cmd);
    void publish_log_collect_error(const LogCollectCommand& cmd, const char* reason);
    void publish_log_collect_accepted(const LogCollectCommand& cmd,
                                      int64_t file_size,
                                      const std::vector<LogFileReport>& reports);
    void publish_log_collect_progress(const std::string& collect_id,
                                      const std::string& attempt_id,
                                      int64_t transferred_bytes,
                                      int64_t total_bytes);
    bool subscribe_ota_topics();
    void handle_ota_notify(const std::string& payload);
    void handle_ota_manifest_response(const std::string& payload);
    bool parse_ota_notify(const std::string& payload, OtaNotify& out) const;
    bool parse_manifest_response(const std::string& payload,
                                 std::string& task_id,
                                 std::vector<OtaComponent>& components,
                                 std::string& err_code) const;
    bool publish_manifest_request(const std::string& task_id);
    bool publish_ota_progress_cloud(const middleware::OtaProgressPayload& progress);
    void publish_ota_progress_ui(const middleware::OtaProgressPayload& progress);
    bool trigger_galvo_real_ota(const std::string& task_id, const std::string& component);
    bool wait_galvo_real_ota(std::string& err_code);
    void run_manifest_download_task(std::string task_id, std::vector<OtaComponent> components);
    void start_manifest_download(std::string task_id, std::vector<OtaComponent> components);
    void request_firmware_versions_force(bool need_mcu, bool need_galvo);
    void poll_ota_heartbeat_wait();
    static bool parse_mcu_version_text(const std::string& text,
                                       middleware::McuVersionPayload& out);
    static bool parse_galvo_version_text(const std::string& text,
                                         middleware::GalvoVersionPayload& out);
    static bool json_extract_bool(const std::string& json, const char* key, bool& out);
    static bool json_extract_array_objects(const std::string& json, const char* key,
                                           std::vector<std::string>& out);
    static std::string topic_from_incoming(const char* topic_name, int topic_len);
    static bool map_approval_status(const std::string& text,
                                    middleware::OtaApprovalStatus& out);

    static int on_message_arrived(void* context, char* topic_name,
                                  int topic_len, MQTTClient_message* message);
    static uint64_t monotonic_ms();
    static int64_t unix_ms();
    static std::string read_boot_id();
    static int64_t read_uptime_s();
    static int bars_to_rssi_dbm(int bars, bool connected);
    static std::string format_mcu_version(const middleware::McuVersionPayload& p);
    static std::string format_galvo_version(uint32_t fw_version_le);

    static bool json_extract_string(const std::string& json, const char* key,
                                    std::string& out);
    static bool json_extract_int(const std::string& json, const char* key, int& out);
    static bool json_extract_int64(const std::string& json, const char* key, int64_t& out);
    /** 提取字符串数组（如 topicFiles）。 */
    static bool json_extract_string_array(const std::string& json, const char* key,
                                          std::vector<std::string>& out);
    /** 提取指定 JSON 对象（包含首尾花括号），用于严格解析命令根节点的 data。 */
    static bool json_extract_object(const std::string& json, const char* key,
                                    std::string& out);
    static std::string json_escape(const std::string& text);
    template <typename T>
    static bool read_inline_pod(const middleware::MsgPayload& payload, T& out)
    {
        if(payload.type != middleware::MsgPayload::Type::Inline ||
           payload.length < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, payload.data.bytes.data(), sizeof(T));
        return true;
    }

    MqttConfig cfg_{};
    std::vector<middleware::Subscription> subs_;
    MqttCredentialStore credential_store_{""};
    bool credentials_ready_ = false;
    uint64_t next_activation_ms_ = 0;
    uint32_t activation_fail_count_ = 0;
    std::thread activation_thread_;
    std::mutex activation_mtx_;
    bool activation_running_ = false;
    bool activation_result_ready_ = false;
    bool activation_succeeded_ = false;
    ActivatedMqttCredential activated_credential_{};
    std::string activation_error_;

    MQTTClient client_{nullptr};
    bool mqtt_created_ = false;
    bool mqtt_connected_ = false;
    bool cmd_subscribed_ = false;
    bool ota_subscribed_ = false;
    uint64_t last_reconnect_ms_ = 0;

    std::mutex data_mtx_;
    ChamberTelemetry latest_{};
    FirmwareCache fw_cache_{};
    bool env_sim_on_ = false;
    std::string soc_fw_override_;
    struct OtaHeartbeatWait {
        bool active = false;
        bool need_mcu = false;
        bool need_galvo = false;
        bool got_mcu = false;
        bool got_galvo = false;
        uint64_t deadline_ms = 0;
    } ota_hb_wait_{};
    uint64_t last_telemetry_pub_ms_ = 0;
    uint64_t last_heartbeat_pub_ms_ = 0;
    uint64_t last_fw_query_ms_ = 0;
    std::string boot_id_;

    std::mutex pending_mtx_;
    std::unordered_map<std::string, PendingAck> pending_acks_;

    std::mutex ota_mtx_;
    OtaNotify pending_ota_notify_{};
    std::thread ota_download_thread_;
    bool ota_download_running_ = false;
    struct GalvoOtaSync {
        bool waiting = false;
        bool finished = false;
        bool success = false;
        bool terminal_seen = false;
        bool last_apply_pct_valid = false;
        uint8_t last_apply_pct = 0;
        bool last_mcu_status_valid = false;
        middleware::McuOtaState last_mcu_state = middleware::McuOtaState::Idle;
        uint8_t last_mcu_progress = 0;
        std::string task_id;
        std::string component;
        std::string error_code;
    } galvo_ota_sync_{};
    std::mutex galvo_ota_mtx_;
    std::condition_variable galvo_ota_cv_;

    std::mutex log_collect_mtx_;
    std::thread log_collect_thread_;
    bool log_collect_running_ = false;
    uint64_t last_log_collect_attempt_ms_ = 0;

    std::atomic<uint32_t> seq_{0};
};

} // namespace upc::mqtt

#endif
