#include "mqtt_config.hpp"

#include <cstdlib>

namespace upc::mqtt {

namespace {

std::string get_env_or_default(const char* key, const char* fallback)
{
    const char* v = std::getenv(key);
    if(v && v[0]) {
        return std::string(v);
    }
    return std::string(fallback ? fallback : "");
}

int get_env_int_or_default(const char* key, int fallback)
{
    const char* v = std::getenv(key);
    if(!v || !v[0]) {
        return fallback;
    }
    const int parsed = std::atoi(v);
    return parsed > 0 ? parsed : fallback;
}

} // namespace

std::string MqttConfig::topic_prefix() const
{
    return "snowpod/" + region + "/" + device_id + "/";
}

std::string MqttConfig::topic_cmd() const
{
    return topic_prefix() + "cmd";
}

std::string MqttConfig::topic_cmd_ack() const
{
    return topic_prefix() + "cmd/ack";
}

std::string MqttConfig::topic_telemetry_chamber() const
{
    return topic_prefix() + "telemetry/chamber";
}

MqttConfig load_mqtt_config()
{
    MqttConfig cfg;
    cfg.region = get_env_or_default("SNOWPOD_MQTT_REGION", "cn");
    cfg.device_id = get_env_or_default("SNOWPOD_MQTT_DEVICE_ID", "SNOWPOD-00000000");
    cfg.broker_uri = get_env_or_default("SNOWPOD_MQTT_BROKER_URI", "tcp://127.0.0.1:1883");
    cfg.client_id = get_env_or_default("SNOWPOD_MQTT_CLIENT_ID", "");
    cfg.username = get_env_or_default("SNOWPOD_MQTT_USERNAME", "");
    cfg.password = get_env_or_default("SNOWPOD_MQTT_PASSWORD", "");
    cfg.keepalive_sec = get_env_int_or_default("SNOWPOD_MQTT_KEEPALIVE_SEC", 60);
    cfg.qos_telemetry = get_env_int_or_default("SNOWPOD_MQTT_QOS_TELEMETRY", 1);
    cfg.qos_ack = get_env_int_or_default("SNOWPOD_MQTT_QOS_ACK", 1);
    cfg.clean_session = get_env_int_or_default("SNOWPOD_MQTT_CLEAN_SESSION", 1) != 0;

    if(cfg.client_id.empty()) {
        cfg.client_id = "snowpod-" + cfg.device_id;
    }
    if(cfg.qos_telemetry < 0 || cfg.qos_telemetry > 1) {
        cfg.qos_telemetry = 1;
    }
    if(cfg.qos_ack < 0 || cfg.qos_ack > 1) {
        cfg.qos_ack = 1;
    }
    return cfg;
}

} // namespace upc::mqtt
