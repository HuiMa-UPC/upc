#ifndef UPC_MQTT_SERVICE_HPP
#define UPC_MQTT_SERVICE_HPP

#include "mqtt_config.hpp"
#include "upc_contract.hpp"
#include "msg_port.hpp"

#include <MQTTClient.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
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
        bool has_value = false;
    };

    struct LightCommand {
        std::string req_id;
        std::string mode;
        int brightness = -1;
    };

    void subscribe_messages();
    void on_env_update(const middleware::Message& msg);
    void cache_telemetry(const middleware::EnvSnapshotPayload& p);
    bool read_env_payload(const middleware::Message& msg,
                          middleware::EnvSnapshotPayload& out) const;

    bool ensure_mqtt_connected();
    bool connect_mqtt();
    void disconnect_mqtt();
    void try_reconnect();
    bool subscribe_cmd_topic();

    bool publish_chamber_telemetry();
    bool publish_cmd_ack(const std::string& req_id,
                         const std::string& cmd,
                         const std::string& result,
                         const char* reason,
                         const char* detail_json = "{}");

    void handle_command_message(const std::string& payload);
    bool parse_light_command(const std::string& payload,
                             LightCommand& out,
                             std::string& reject_reason);
    bool dispatch_set_light_to_glavo(const LightCommand& cmd);

    static int on_message_arrived(void* context, char* topic_name,
                                  int topic_len, MQTTClient_message* message);
    static uint64_t monotonic_ms();
    static int64_t unix_ms();

    static bool json_extract_string(const std::string& json, const char* key,
                                    std::string& out);
    static bool json_extract_int(const std::string& json, const char* key, int& out);
    static std::string json_escape(const std::string& text);

    MqttConfig cfg_{};
    std::vector<middleware::Subscription> subs_;

    MQTTClient client_{nullptr};
    bool mqtt_created_ = false;
    bool mqtt_connected_ = false;
    bool cmd_subscribed_ = false;
    uint64_t last_reconnect_ms_ = 0;

    std::mutex data_mtx_;
    ChamberTelemetry latest_{};
    uint64_t last_telemetry_pub_ms_ = 0;

    std::atomic<uint32_t> seq_{0};
};

} // namespace upc::mqtt

#endif
