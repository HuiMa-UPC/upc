#include "mqtt_service.hpp"

#include "rs_log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>

namespace upc::mqtt {

namespace {

constexpr uint64_t kReconnectIntervalMs = 3000;
constexpr uint64_t kTelemetryPrintingIntervalMs = 2000;
constexpr uint64_t kTelemetryIdleIntervalMs = 30000;

constexpr const char* kCmdSetLight = "set_light";
constexpr const char* kCmdRejectReasonInvalidState = "invalid_state";

} // namespace

MqttService::MqttService() = default;

MqttService::~MqttService()
{
    shutdown();
}

bool MqttService::init()
{
    cfg_ = load_mqtt_config();
    subscribe_messages();

    UPC_LOG_MQTT_SERVICE(Info,
        "cfg broker=%s prefix=%s client=%s qos(tele=%d ack=%d)",
        cfg_.broker_uri.c_str(),
        cfg_.topic_prefix().c_str(),
        cfg_.client_id.c_str(),
        cfg_.qos_telemetry,
        cfg_.qos_ack);

    return true;
}

void MqttService::shutdown()
{
    subs_.clear();
    disconnect_mqtt();
    if(mqtt_created_) {
        MQTTClient_destroy(&client_);
        mqtt_created_ = false;
        client_ = nullptr;
    }
}

void MqttService::tick()
{
    if(!ensure_mqtt_connected()) {
        return;
    }

    MQTTClient_yield();
    (void)publish_chamber_telemetry();
}

void MqttService::subscribe_messages()
{
    auto& bus = middleware::MsgPort::instance();
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Update_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_env_update(msg); }));
}

void MqttService::on_env_update(const middleware::Message& msg)
{
    middleware::EnvSnapshotPayload p{};
    if(!read_env_payload(msg, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid APP_Update_Ntf payload");
        return;
    }
    cache_telemetry(p);
}

void MqttService::cache_telemetry(const middleware::EnvSnapshotPayload& p)
{
    std::lock_guard<std::mutex> lock(data_mtx_);
    latest_.pm25 = p.pm25_cabin;
    latest_.oxygen_percent = p.oxygen_percent;
    latest_.temperature_c = p.temperature_c;
    latest_.status = p.status;
    latest_.has_value = true;
}

bool MqttService::read_env_payload(const middleware::Message& msg,
                                   middleware::EnvSnapshotPayload& out) const
{
    if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length < sizeof(out)) {
        return false;
    }
    std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(out));
    return true;
}

bool MqttService::ensure_mqtt_connected()
{
    if(mqtt_connected_ && MQTTClient_isConnected(client_)) {
        return true;
    }

    mqtt_connected_ = false;
    cmd_subscribed_ = false;
    try_reconnect();
    return mqtt_connected_;
}

void MqttService::try_reconnect()
{
    const uint64_t now = monotonic_ms();
    if(last_reconnect_ms_ != 0 &&
       now - last_reconnect_ms_ < kReconnectIntervalMs) {
        return;
    }
    last_reconnect_ms_ = now;
    (void)connect_mqtt();
}

bool MqttService::connect_mqtt()
{
    if(!mqtt_created_) {
        const int rc = MQTTClient_create(
            &client_,
            cfg_.broker_uri.c_str(),
            cfg_.client_id.c_str(),
            MQTTCLIENT_PERSISTENCE_NONE,
            nullptr);
        if(rc != MQTTCLIENT_SUCCESS) {
            UPC_LOG_MQTT_SERVICE(Error, "MQTT create failed rc=%d", rc);
            return false;
        }
        mqtt_created_ = true;
        MQTTClient_setCallbacks(client_, this, nullptr, &MqttService::on_message_arrived, nullptr);
    }

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    opts.keepAliveInterval = cfg_.keepalive_sec;
    opts.cleansession = cfg_.clean_session ? 1 : 0;
    if(!cfg_.username.empty()) {
        opts.username = cfg_.username.c_str();
    }
    if(!cfg_.password.empty()) {
        opts.password = cfg_.password.c_str();
    }

    const int rc = MQTTClient_connect(client_, &opts);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "MQTT connect failed rc=%d uri=%s",
            rc, cfg_.broker_uri.c_str());
        mqtt_connected_ = false;
        return false;
    }

    mqtt_connected_ = true;
    if(!subscribe_cmd_topic()) {
        disconnect_mqtt();
        return false;
    }

    UPC_LOG_MQTT_SERVICE(Info, "MQTT connected uri=%s", cfg_.broker_uri.c_str());
    return true;
}

bool MqttService::subscribe_cmd_topic()
{
    const std::string topic = cfg_.topic_cmd();
    const int rc = MQTTClient_subscribe(client_, topic.c_str(), 1);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Error, "MQTT subscribe failed rc=%d topic=%s",
            rc, topic.c_str());
        return false;
    }
    cmd_subscribed_ = true;
    UPC_LOG_MQTT_SERVICE(Info, "MQTT subscribed topic=%s", topic.c_str());
    return true;
}

void MqttService::disconnect_mqtt()
{
    if(mqtt_created_ && MQTTClient_isConnected(client_)) {
        MQTTClient_disconnect(client_, 1000);
    }
    mqtt_connected_ = false;
    cmd_subscribed_ = false;
}

bool MqttService::publish_chamber_telemetry()
{
    ChamberTelemetry snap{};
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        snap = latest_;
    }
    if(!snap.has_value) {
        return false;
    }

    const bool printing = snap.status ==
        static_cast<uint32_t>(middleware::UserHomeStatus::Printing);
    const uint64_t interval_ms = printing
        ? kTelemetryPrintingIntervalMs
        : kTelemetryIdleIntervalMs;

    const uint64_t now = monotonic_ms();
    if(last_telemetry_pub_ms_ != 0 &&
       now - last_telemetry_pub_ms_ < interval_ms) {
        return false;
    }
    last_telemetry_pub_ms_ = now;

    int o2_ppm = static_cast<int>(std::lround(static_cast<double>(snap.oxygen_percent) * 10000.0));
    if(o2_ppm < 0) {
        o2_ppm = 0;
    }

    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const std::string escaped_device = json_escape(cfg_.device_id);

    char payload[768];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"telemetry\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"data\":{"
            "\"group\":\"chamber\","
            "\"pm25\":%.3f,"
            "\"o2_ppm\":%d,"
            "\"dp_pa\":null,"
            "\"ir_temp_c\":null,"
            "\"ntc_c\":%.3f,"
            "\"smoke\":null"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        static_cast<double>(snap.pm25),
        o2_ppm,
        static_cast<double>(snap.temperature_c));

    const std::string topic = cfg_.topic_telemetry_chamber();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
    msg.qos = cfg_.qos_telemetry;
    msg.retained = 0;

    MQTTClient_deliveryToken token;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish telemetry fail rc=%d topic=%s",
            rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }
    return true;
}

bool MqttService::publish_cmd_ack(const std::string& req_id,
                                  const std::string& cmd,
                                  const std::string& result,
                                  const char* reason,
                                  const char* detail_json)
{
    if(req_id.empty()) {
        UPC_LOG_MQTT_SERVICE(Warn, "skip cmd/ack because req_id empty");
        return false;
    }

    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const std::string escaped_device = json_escape(cfg_.device_id);
    const std::string escaped_req = json_escape(req_id);
    const std::string escaped_cmd = json_escape(cmd);
    const std::string escaped_result = json_escape(result);
    const std::string reason_json = reason
        ? ("\"" + json_escape(reason) + "\"")
        : "null";
    const char* detail = detail_json ? detail_json : "{}";

    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"command_ack\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"req_id\":\"%s\","
        "\"data\":{"
            "\"cmd\":\"%s\","
            "\"result\":\"%s\","
            "\"reason\":%s,"
            "\"detail\":%s"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        escaped_req.c_str(),
        escaped_cmd.c_str(),
        escaped_result.c_str(),
        reason_json.c_str(),
        detail);

    const std::string topic = cfg_.topic_cmd_ack();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
    msg.qos = cfg_.qos_ack;
    msg.retained = 0;

    MQTTClient_deliveryToken token;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish cmd/ack fail rc=%d topic=%s",
            rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }
    return true;
}

void MqttService::handle_command_message(const std::string& payload)
{
    LightCommand light{};
    std::string reject_reason;
    std::string cmd_name;
    std::string req_id;
    (void)json_extract_string(payload, "cmd", cmd_name);
    (void)json_extract_string(payload, "req_id", req_id);

    if(!parse_light_command(payload, light, reject_reason)) {
        if(!req_id.empty()) {
            const std::string ack_cmd = cmd_name.empty() ? kCmdSetLight : cmd_name;
            (void)publish_cmd_ack(req_id, ack_cmd, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str());
        }
        return;
    }

    if(dispatch_set_light_to_glavo(light)) {
        (void)publish_cmd_ack(light.req_id, kCmdSetLight, "accepted", nullptr);
    } else {
        (void)publish_cmd_ack(light.req_id, kCmdSetLight, "error", nullptr);
    }
}

bool MqttService::parse_light_command(const std::string& payload,
                                      LightCommand& out,
                                      std::string& reject_reason)
{
    reject_reason.clear();

    std::string type;
    if(!json_extract_string(payload, "type", type) || type != "command") {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    std::string req_id;
    if(!json_extract_string(payload, "req_id", req_id) || req_id.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    std::string device_id;
    if(!json_extract_string(payload, "device_id", device_id) ||
       device_id != cfg_.device_id) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    std::string cmd;
    if(!json_extract_string(payload, "cmd", cmd) || cmd != kCmdSetLight) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    std::string zone;
    if(!json_extract_string(payload, "zone", zone) || zone != "chamber") {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    std::string mode;
    if(!json_extract_string(payload, "mode", mode) ||
       (mode != "on" && mode != "off")) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    int brightness = -1;
    if(json_extract_int(payload, "brightness", brightness)) {
        if(brightness < 0 || brightness > 100) {
            reject_reason = kCmdRejectReasonInvalidState;
            return false;
        }
    }

    out.req_id = req_id;
    out.mode = mode;
    out.brightness = brightness;
    return true;
}

bool MqttService::dispatch_set_light_to_glavo(const LightCommand& cmd)
{
    middleware::DebugDevicePayload p{};
    p.target = static_cast<uint8_t>(middleware::DebugDeviceTarget::Light);
    p.on = cmd.mode == "on" ? 1 : 0;

    middleware::Message msg(middleware::MsgType::UI_Debug_Device_Req,
        middleware::ModuleId::APP_MQTT,
        middleware::ModuleId::APP_GLAVO);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));

    const bool ok = middleware::MsgPort::instance().publish_async(msg);
    if(!ok) {
        UPC_LOG_MQTT_SERVICE(Warn, "dispatch set_light failed");
    }
    return ok;
}

int MqttService::on_message_arrived(void* context, char* topic_name,
                                    int /*topic_len*/, MQTTClient_message* message)
{
    if(!context || !message || !message->payload) {
        if(message) {
            MQTTClient_freeMessage(&message);
        }
        if(topic_name) {
            MQTTClient_free(topic_name);
        }
        return 1;
    }

    auto* self = static_cast<MqttService*>(context);
    const std::string payload(
        static_cast<const char*>(message->payload),
        static_cast<size_t>(message->payloadlen));
    self->handle_command_message(payload);

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topic_name);
    return 1;
}

uint64_t MqttService::monotonic_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

int64_t MqttService::unix_ms()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

bool MqttService::json_extract_string(const std::string& json, const char* key,
                                      std::string& out)
{
    out.clear();
    if(!key || !key[0]) {
        return false;
    }
    const std::string token = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(token);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', key_pos + token.size());
    if(pos == std::string::npos) {
        return false;
    }
    ++pos;
    while(pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if(pos >= json.size() || json[pos] != '"') {
        return false;
    }
    ++pos;
    std::string value;
    bool escaped = false;
    for(; pos < json.size(); ++pos) {
        const char c = json[pos];
        if(escaped) {
            value.push_back(c);
            escaped = false;
            continue;
        }
        if(c == '\\') {
            escaped = true;
            continue;
        }
        if(c == '"') {
            out = value;
            return true;
        }
        value.push_back(c);
    }
    return false;
}

bool MqttService::json_extract_int(const std::string& json, const char* key, int& out)
{
    const std::string token = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(token);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', key_pos + token.size());
    if(pos == std::string::npos) {
        return false;
    }
    ++pos;
    while(pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if(pos >= json.size()) {
        return false;
    }
    size_t end = pos;
    if(json[end] == '-') {
        ++end;
    }
    while(end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) {
        ++end;
    }
    if(end == pos || (end == pos + 1 && json[pos] == '-')) {
        return false;
    }
    out = std::atoi(json.substr(pos, end - pos).c_str());
    return true;
}

std::string MqttService::json_escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for(char c : text) {
        switch(c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace upc::mqtt
