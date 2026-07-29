#ifndef UPC_MQTT_CONFIG_HPP
#define UPC_MQTT_CONFIG_HPP

#include <string>

namespace upc::mqtt {

struct MqttConfig {
    std::string region = "cn";
    std::string device_id = "SNOWPOD-00000000";
    std::string broker_uri = "tcp://127.0.0.1:1883";
    std::string client_id = "snowpod-client";
    std::string username;
    std::string password;
    int keepalive_sec = 60;
    int qos_telemetry = 1;
    int qos_ack = 1;
    bool clean_session = true;

    [[nodiscard]] std::string topic_prefix() const;
    [[nodiscard]] std::string topic_cmd() const;
    [[nodiscard]] std::string topic_cmd_ack() const;
    [[nodiscard]] std::string topic_telemetry_chamber() const;
};

MqttConfig load_mqtt_config();

} // namespace upc::mqtt

#endif
