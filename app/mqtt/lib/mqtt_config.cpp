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
    return "snowpod/cn/" + device_id + "/";
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

std::string MqttConfig::topic_heartbeat() const
{
    return topic_prefix() + "heartbeat";
}

MqttConfig load_mqtt_config()
{
    MqttConfig cfg;
    cfg.device_id = get_env_or_default("SNOWPOD_MQTT_DEVICE_ID", "SNOWPOD-050986DC98001000");
    cfg.device_secret = get_env_or_default("SNOWPOD_DEVICE_SECRET", "bacff0d7cc54115f6c268e4eabc5538670346c2a084037079796f50abd52e985");
    cfg.client_id = get_env_or_default("SNOWPOD_MQTT_CLIENT_ID", "");
    cfg.username = get_env_or_default("SNOWPOD_MQTT_USERNAME", "");
    cfg.activation_base_url = get_env_or_default(
        "SNOWPOD_DEVICE_INIT_BASE_URL", "http://172.17.13.31:8081");
    cfg.credential_file = get_env_or_default(
        "SNOWPOD_MQTT_CREDENTIAL_FILE", "/data/snowpod/mqtt-credential");
    cfg.keepalive_sec = get_env_int_or_default("SNOWPOD_MQTT_KEEPALIVE_SEC", 60);
    cfg.qos_telemetry = get_env_int_or_default("SNOWPOD_MQTT_QOS_TELEMETRY", 1);
    cfg.qos_heartbeat = get_env_int_or_default("SNOWPOD_MQTT_QOS_HEARTBEAT", 1);
    cfg.qos_ack = get_env_int_or_default("SNOWPOD_MQTT_QOS_ACK", 1);
    cfg.heartbeat_interval_sec = get_env_int_or_default("SNOWPOD_MQTT_HEARTBEAT_INTERVAL_SEC", 30);
    cfg.clean_session = get_env_int_or_default("SNOWPOD_MQTT_CLEAN_SESSION", 1) != 0;

    if(cfg.client_id.empty()) {
        cfg.client_id = cfg.device_id;
    }
    if(cfg.username.empty()) {
        cfg.username = cfg.device_id;
    }
    if(cfg.qos_telemetry < 0 || cfg.qos_telemetry > 1) {
        cfg.qos_telemetry = 1;
    }
    if(cfg.qos_heartbeat < 0 || cfg.qos_heartbeat > 1) {
        cfg.qos_heartbeat = 1;
    }
    if(cfg.qos_ack < 0 || cfg.qos_ack > 1) {
        cfg.qos_ack = 1;
    }
    if(cfg.heartbeat_interval_sec <= 0) {
        cfg.heartbeat_interval_sec = 30;
    }
    return cfg;
}

} // namespace upc::mqtt
