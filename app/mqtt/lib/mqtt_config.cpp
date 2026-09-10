#include "mqtt_config.hpp"

#include <cstdlib>
#include <fstream>

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

std::string trim_ascii(const std::string& in)
{
    size_t begin = 0;
    while(begin < in.size() &&
          (in[begin] == ' ' || in[begin] == '\t' ||
           in[begin] == '\r' || in[begin] == '\n')) {
        ++begin;
    }
    size_t end = in.size();
    while(end > begin &&
          (in[end - 1] == ' ' || in[end - 1] == '\t' ||
           in[end - 1] == '\r' || in[end - 1] == '\n')) {
        --end;
    }
    return in.substr(begin, end - begin);
}

std::string strip_outer_quotes(const std::string& in)
{
    const std::string t = trim_ascii(in);
    if(t.size() >= 2 && t.front() == '"' && t.back() == '"') {
        return t.substr(1, t.size() - 2);
    }
    return t;
}

bool read_device_key_file(const std::string& path,
                          std::string& out_device_id,
                          std::string& out_device_secret)
{
    out_device_id.clear();
    out_device_secret.clear();
    std::ifstream in(path);
    if(!in) {
        return false;
    }
    std::string line1;
    std::string line2;
    if(!std::getline(in, line1)) {
        return false;
    }
    if(!std::getline(in, line2)) {
        return false;
    }
    out_device_id = strip_outer_quotes(line1);
    out_device_secret = strip_outer_quotes(line2);
    return !out_device_id.empty() && !out_device_secret.empty();
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

std::string MqttConfig::topic_state() const
{
    return topic_prefix() + "state";
}

std::string MqttConfig::topic_ota_notify() const
{
    return topic_prefix() + "ota/notify";
}

std::string MqttConfig::topic_ota_manifest_req() const
{
    return topic_prefix() + "ota/manifest/request";
}

std::string MqttConfig::topic_ota_manifest_resp() const
{
    return topic_prefix() + "ota/manifest";
}

std::string MqttConfig::topic_ota_progress() const
{
    return topic_prefix() + "ota/progress";
}

std::string MqttConfig::topic_log_collect_progress() const
{
    return topic_prefix() + "log_collect/progress";
}

//桌面调试
// "SNOWPOD-050986DC98001000";
// "bacff0d7cc54115f6c268e4eabc5538670346c2a084037079796f50abd52e985"
//营销3
// "SNOWPOD-050A8F7EFF001000"
//  "7cf74dfbe68ac0941afba3a89c60b2476f6e20b8815070ccd9f158a28608d370"
//9
 // "SNOWPOD-050A8F80DD001000"
 // "d384efe6bd259bc4b5d471641b0611486a16fae05404dddb68c78428dbe8f009"
//Guobin
//"SNOWPOD-050A8F7EFF001000"
//"7cf74dfbe68ac0941afba3a89c60b2476f6e20b8815070ccd9f158a28608d370"



MqttConfig load_mqtt_config()
{
    MqttConfig cfg;
    const std::string key_file = get_env_or_default("SNOWPOD_DEVICE_KEY_FILE", "/userdata/snowpod/key");
    std::string file_device_id;
    std::string file_device_secret;
    (void)read_device_key_file(key_file, file_device_id, file_device_secret);

    cfg.device_id = get_env_or_default("SNOWPOD_MQTT_DEVICE_ID", file_device_id.c_str());
    cfg.device_secret = get_env_or_default("SNOWPOD_DEVICE_SECRET", file_device_secret.c_str());
    cfg.client_id = get_env_or_default("SNOWPOD_MQTT_CLIENT_ID", "");
    cfg.username = get_env_or_default("SNOWPOD_MQTT_USERNAME", "");
    cfg.activation_base_url = get_env_or_default(
        "SNOWPOD_DEVICE_INIT_BASE_URL", "http://172.17.13.31:8081");
    cfg.credential_file = get_env_or_default(
        "SNOWPOD_MQTT_CREDENTIAL_FILE", "/userdata/snowpod/mqtt-credential");
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
