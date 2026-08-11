#ifndef UPC_MQTT_SERVICE_HPP
#define UPC_MQTT_SERVICE_HPP

#include "mqtt_config.hpp"
#include "device_activation.hpp"
#include "upc_contract.hpp"
#include "msg_port.hpp"

#include <MQTTClient.h>

#include <atomic>
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

private:
    struct ChamberTelemetry {
        float pm25 = 0.0f;
        float oxygen_percent = 0.0f;
        float temperature_c = 0.0f;
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

    void subscribe_messages();
    void on_env_update(const middleware::Message& msg);
    void on_rtsp_start_rsp(const middleware::Message& msg);
    void on_rtsp_stop_rsp(const middleware::Message& msg);
    void cache_telemetry(const middleware::EnvSnapshotPayload& p);
    bool read_env_payload(const middleware::Message& msg,
                          middleware::EnvSnapshotPayload& out) const;
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
    bool dispatch_set_light_to_glavo(const LightCommand& cmd);
    bool dispatch_rtsp_start_to_media(const RtspCommand& cmd);
    bool dispatch_rtsp_stop_to_media(const RtspCommand& cmd);
    void handle_rtsp_rsp(const middleware::Message& msg, const char* expect_cmd);
    bool track_pending_ack(const std::string& req_id, const std::string& cmd);

    static int on_message_arrived(void* context, char* topic_name,
                                  int topic_len, MQTTClient_message* message);
    static uint64_t monotonic_ms();
    static int64_t unix_ms();
    static std::string read_boot_id();
    static int64_t read_uptime_s();
    static int bars_to_rssi_dbm(int bars, bool connected);

    static bool json_extract_string(const std::string& json, const char* key,
                                    std::string& out);
    static bool json_extract_int(const std::string& json, const char* key, int& out);
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
    uint64_t last_reconnect_ms_ = 0;

    std::mutex data_mtx_;
    ChamberTelemetry latest_{};
    uint64_t last_telemetry_pub_ms_ = 0;
    uint64_t last_heartbeat_pub_ms_ = 0;
    std::string boot_id_;

    std::mutex pending_mtx_;
    std::unordered_map<std::string, PendingAck> pending_acks_;

    std::atomic<uint32_t> seq_{0};
};

} // namespace upc::mqtt

#endif
