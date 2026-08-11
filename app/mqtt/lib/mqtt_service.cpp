#include "mqtt_service.hpp"

#include "rs_log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace upc::mqtt {

namespace {

constexpr uint64_t kReconnectIntervalMs = 3000;
constexpr uint64_t kActivationRetryIntervalMs = 5000;
constexpr uint64_t kTelemetryPrintingIntervalMs = 2000;
constexpr uint64_t kTelemetryIdleIntervalMs = 30000;

constexpr const char* kCmdSetLight = "set_light";
constexpr const char* kCmdStartRtspPush = "start_rtsp_push";
constexpr const char* kCmdStopRtspPush = "stop_rtsp_push";
constexpr const char* kCmdRejectReasonInvalidState = "invalid_state";
constexpr const char* kCmdRejectReasonEncoderError = "encoder_error";
constexpr uint64_t kRtspAckTimeoutMs = 8000;

#ifndef UPC_SW_VERSION
#define UPC_SW_VERSION "unknown"
#endif

#if UPC_DEBUG && UPC_MOD_MQTT_SERVICE
/**
 * @brief 打印 MQTT 收发原始帧。
 *
 * JSON 可打印字符保持原样，控制字符/二进制字节转义为 \xNN；分块限制单条日志长度，
 * 避免超过 rs_log 的消息缓冲区。该函数完全受 UPC_MOD_MQTT_SERVICE 控制。
 */
void debug_log_mqtt_frame(const char* direction,
                          const std::string& topic,
                          const void* payload,
                          size_t payload_len,
                          int qos,
                          int retained)
{
    const char* dir = direction ? direction : "?";
    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT %s frame topic=%s qos=%d retained=%d len=%zu",
        dir, topic.c_str(), qos, retained, payload_len);

    if(!payload || payload_len == 0) {
        UPC_LOG_MQTT_SERVICE(Debug, "MQTT %s payload=<empty>", dir);
        return;
    }

    constexpr size_t kMaxEncodedChunk = 1200;
    const auto* bytes = static_cast<const unsigned char*>(payload);
    size_t offset = 0;
    while(offset < payload_len) {
        const size_t begin = offset;
        std::string encoded;
        encoded.reserve(kMaxEncodedChunk);

        while(offset < payload_len) {
            const unsigned char c = bytes[offset];
            char escaped[5]{};
            const char* token = escaped;
            size_t token_len = 0;

            switch(c) {
            case '\\': token = "\\\\"; token_len = 2; break;
            case '"':  token = "\\\""; token_len = 2; break;
            case '\n': token = "\\n";  token_len = 2; break;
            case '\r': token = "\\r";  token_len = 2; break;
            case '\t': token = "\\t";  token_len = 2; break;
            default:
                if(c >= 0x20 && c <= 0x7e) {
                    escaped[0] = static_cast<char>(c);
                    token_len = 1;
                } else {
                    std::snprintf(escaped, sizeof(escaped), "\\x%02X",
                                  static_cast<unsigned>(c));
                    token_len = 4;
                }
                break;
            }

            if(!encoded.empty() && encoded.size() + token_len > kMaxEncodedChunk) {
                break;
            }
            encoded.append(token, token_len);
            ++offset;
        }

        UPC_LOG_MQTT_SERVICE(Debug,
            "MQTT %s payload[%zu..%zu]=\"%s\"",
            dir, begin, offset, encoded.c_str());
    }
}
#else
inline void debug_log_mqtt_frame(const char*, const std::string&, const void*,
                                 size_t, int, int)
{
}
#endif

} // namespace

MqttService::MqttService() = default;

MqttService::~MqttService()
{
    shutdown();
}

bool MqttService::init()
{
    cfg_ = load_mqtt_config();
    boot_id_ = read_boot_id();
    credential_store_ = MqttCredentialStore(cfg_.credential_file);

    ActivatedMqttCredential saved_credential;
    std::string credential_error;
    if(credential_store_.load(saved_credential, credential_error)) {
        cfg_.password = saved_credential.mqtt_password;
        cfg_.broker_uri = normalise_broker_uri(saved_credential.mqtt_endpoint);
        credentials_ready_ = true;
        UPC_LOG_MQTT_SERVICE(Info, "loaded locally activated MQTT credential");
    } else if(!credential_error.empty()) {
        UPC_LOG_MQTT_SERVICE(Warn, "cannot load local MQTT credential: %s",
                             credential_error.c_str());
    }

    subscribe_messages();

    UPC_LOG_MQTT_SERVICE(Info,
        "cfg broker=%s prefix=%s client=%s user=%s qos(tele=%d hb=%d ack=%d) hb=%ds credential=%s",
        cfg_.broker_uri.c_str(),
        cfg_.topic_prefix().c_str(),
        cfg_.client_id.c_str(),
        cfg_.username.c_str(),
        cfg_.qos_telemetry,
        cfg_.qos_heartbeat,
        cfg_.qos_ack,
        cfg_.heartbeat_interval_sec,
        credentials_ready_ ? "ready" : "activation-required");

    return true;
}

void MqttService::shutdown()
{
    join_activation_thread();
    {
        std::lock_guard<std::mutex> lock(pending_mtx_);
        pending_acks_.clear();
    }
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
    if(!credentials_ready_) {
        poll_activation();
        return;
    }
    if(!ensure_mqtt_connected()) {
        return;
    }

    MQTTClient_yield();
    (void)publish_chamber_telemetry();
    (void)publish_heartbeat();
    expire_pending_acks();
}

void MqttService::subscribe_messages()
{
    auto& bus = middleware::MsgPort::instance();
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Update_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_env_update(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Mqtt_Rtsp_Start_Rsp,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_rtsp_start_rsp(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Mqtt_Rtsp_Stop_Rsp,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_rtsp_stop_rsp(msg); }));
}

void MqttService::on_env_update(const middleware::Message& msg)
{
    middleware::EnvSnapshotPayload p{};
    if(!read_env_payload(msg, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid APP_Update_Ntf payload");
        return;
    }
    UPC_LOG_MQTT_SERVICE(Debug,
        "IPC RX APP_Update_Ntf status=%u pm25=%.3f o2=%.3f%% temp=%.3fC "
        "humidity=%.3f%% pressure=%.3fkPa",
        static_cast<unsigned>(p.status),
        static_cast<double>(p.pm25_cabin),
        static_cast<double>(p.oxygen_percent),
        static_cast<double>(p.temperature_c),
        static_cast<double>(p.humidity_percent),
        static_cast<double>(p.pressure_kpa));
    cache_telemetry(p);
}

void MqttService::cache_telemetry(const middleware::EnvSnapshotPayload& p)
{
    std::lock_guard<std::mutex> lock(data_mtx_);
    /* 环境指标全为哨兵 <0（上电无数据 / Env Sim 关闭清缓存）→ 停遥测直到下一帧有效数据 */
    const bool env_invalid =
        (p.pm25_cabin < 0.0f) &&
        (p.oxygen_percent < 0.0f) &&
        (p.temperature_c < 0.0f);
    if(env_invalid) {
        latest_.wifi_connected = p.wifi_connected != 0;
        latest_.signal_bars = p.signal_bars;
        latest_.has_value = false;
        return;
    }
    latest_.pm25 = p.pm25_cabin;
    latest_.oxygen_percent = p.oxygen_percent;
    latest_.temperature_c = p.temperature_c;
    latest_.status = p.status;
    latest_.wifi_connected = p.wifi_connected != 0;
    latest_.signal_bars = p.signal_bars;
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

void MqttService::start_activation()
{
    {
        std::lock_guard<std::mutex> lock(activation_mtx_);
        if(activation_running_) {
            return;
        }
        activation_running_ = true;
        activation_result_ready_ = false;
        activation_succeeded_ = false;
        activation_error_.clear();
        activated_credential_ = ActivatedMqttCredential{};
    }

    join_activation_thread();
    const MqttConfig activation_config = cfg_;
    activation_thread_ = std::thread([this, activation_config]() {
        DeviceActivationClient client(activation_config);
        ActivatedMqttCredential credential;
        std::string error;
        const bool succeeded = client.activate(credential, error);
        std::lock_guard<std::mutex> lock(activation_mtx_);
        activation_succeeded_ = succeeded;
        activated_credential_ = std::move(credential);
        activation_error_ = std::move(error);
        activation_running_ = false;
        activation_result_ready_ = true;
    });
}

void MqttService::poll_activation()
{
    bool result_ready = false;
    bool succeeded = false;
    ActivatedMqttCredential credential;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(activation_mtx_);
        result_ready = activation_result_ready_;
        if(result_ready) {
            succeeded = activation_succeeded_;
            credential = activated_credential_;
            error = activation_error_;
            activation_result_ready_ = false;
        }
    }

    if(result_ready) {
        join_activation_thread();
        if(succeeded) {
            std::string save_error;
            if(credential_store_.save(credential, save_error)) {
                cfg_.password = credential.mqtt_password;
                cfg_.broker_uri = normalise_broker_uri(credential.mqtt_endpoint);
                credentials_ready_ = true;
                UPC_LOG_MQTT_SERVICE(Info, "device activation succeeded; MQTT credential saved");
                return;
            }
            error = "激活成功但保存 MQTT 凭据失败: " + save_error;
        }
        next_activation_ms_ = monotonic_ms() + kActivationRetryIntervalMs;
        UPC_LOG_MQTT_SERVICE(Warn, "device activation failed: %s; retry in %llums",
                             error.c_str(),
                             static_cast<unsigned long long>(kActivationRetryIntervalMs));
        return;
    }

    bool activation_running = false;
    {
        std::lock_guard<std::mutex> lock(activation_mtx_);
        activation_running = activation_running_;
    }
    if(!activation_running && monotonic_ms() >= next_activation_ms_) {
        UPC_LOG_MQTT_SERVICE(Info, "starting device activation");
        start_activation();
    }
}

void MqttService::join_activation_thread()
{
    if(activation_thread_.joinable()) {
        activation_thread_.join();
    }
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
// #ifdef MQTTVERSION_5
//     opts.MQTTVersion = MQTTVERSION_5;
// #endif
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
    last_heartbeat_pub_ms_ = 0;
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

    debug_log_mqtt_frame("TX", topic, msg.payload,
                         static_cast<size_t>(msg.payloadlen),
                         msg.qos, msg.retained);

    MQTTClient_deliveryToken token;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish telemetry fail rc=%d topic=%s",
            rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }
    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT TX telemetry queued topic=%s token=%d",
        topic.c_str(), static_cast<int>(token));
    return true;
}

bool MqttService::publish_heartbeat()
{
    const uint64_t now = monotonic_ms();
    const uint64_t interval_ms =
        static_cast<uint64_t>(cfg_.heartbeat_interval_sec) * 1000ULL;
    if(last_heartbeat_pub_ms_ != 0 &&
       now - last_heartbeat_pub_ms_ < interval_ms) {
        return false;
    }

    bool wifi_connected = false;
    int signal_bars = -1;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        wifi_connected = latest_.wifi_connected;
        signal_bars = latest_.signal_bars;
    }

    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const long long uptime_s = read_uptime_s();
    const int rssi = bars_to_rssi_dbm(signal_bars, wifi_connected);
    const char* network_type = wifi_connected ? "wifi" : "none";
    const std::string escaped_device = json_escape(cfg_.device_id);
    const std::string escaped_boot = json_escape(boot_id_);
    const std::string escaped_fw = json_escape(UPC_SW_VERSION);

    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"heartbeat\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"data\":{"
            "\"boot_id\":\"%s\","
            "\"uptime_s\":%lld,"
            "\"firmwares\":[{\"firmware_type\":\"soc\",\"version\":\"%s\"}],"
            "\"battery\":null,"
            "\"network\":{\"type\":\"%s\",\"rssi\":%d}"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        escaped_boot.c_str(),
        uptime_s,
        escaped_fw.c_str(),
        network_type,
        rssi);

    const std::string topic = cfg_.topic_heartbeat();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
    msg.qos = cfg_.qos_heartbeat;
    msg.retained = 0;

    debug_log_mqtt_frame("TX", topic, msg.payload,
                         static_cast<size_t>(msg.payloadlen),
                         msg.qos, msg.retained);

    MQTTClient_deliveryToken token;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish heartbeat fail rc=%d topic=%s",
                             rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }

    last_heartbeat_pub_ms_ = now;
    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT TX heartbeat queued topic=%s token=%d",
        topic.c_str(), static_cast<int>(token));
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

    debug_log_mqtt_frame("TX", topic, msg.payload,
                         static_cast<size_t>(msg.payloadlen),
                         msg.qos, msg.retained);

    MQTTClient_deliveryToken token;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish cmd/ack fail rc=%d topic=%s",
            rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }
    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT TX cmd/ack queued topic=%s token=%d req_id=%s result=%s",
        topic.c_str(), static_cast<int>(token), req_id.c_str(), result.c_str());
    return true;
}

void MqttService::handle_command_message(const std::string& payload)
{
    std::string req_id;
    std::string cmd_name;
    std::string data_json;
    std::string reject_reason;
    if(!parse_common_command(payload, req_id, cmd_name, data_json, reject_reason)) {
        UPC_LOG_MQTT_SERVICE(Debug,
            "MQTT RX command rejected req_id=%s cmd=%s reason=%s",
            req_id.c_str(), cmd_name.c_str(),
            reject_reason.empty() ? kCmdRejectReasonInvalidState
                                  : reject_reason.c_str());
        if(!req_id.empty()) {
            const std::string ack_cmd = cmd_name.empty() ? "command" : cmd_name;
            (void)publish_cmd_ack(req_id, ack_cmd, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str());
        }
        return;
    }

    if(cmd_name == kCmdSetLight) {
        LightCommand light{};
        if(!parse_light_command(data_json, req_id, light, reject_reason)) {
            (void)publish_cmd_ack(req_id, kCmdSetLight, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str());
            return;
        }
        UPC_LOG_MQTT_SERVICE(Debug,
            "MQTT RX command parsed req_id=%s cmd=%s mode=%s brightness=%d",
            light.req_id.c_str(), kCmdSetLight, light.mode.c_str(), light.brightness);
        if(dispatch_set_light_to_glavo(light)) {
            (void)publish_cmd_ack(light.req_id, kCmdSetLight, "accepted", nullptr);
        } else {
            (void)publish_cmd_ack(light.req_id, kCmdSetLight, "error", nullptr);
        }
        return;
    }

    if(cmd_name == kCmdStartRtspPush) {
        RtspCommand rtsp{};
        if(!parse_rtsp_start_command(data_json, req_id, rtsp, reject_reason)) {
            (void)publish_cmd_ack(req_id, kCmdStartRtspPush, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str());
            return;
        }
        if(!track_pending_ack(rtsp.req_id, kCmdStartRtspPush)) {
            (void)publish_cmd_ack(rtsp.req_id, kCmdStartRtspPush, "rejected",
                                  kCmdRejectReasonInvalidState);
            return;
        }
        if(!dispatch_rtsp_start_to_media(rtsp)) {
            {
                std::lock_guard<std::mutex> lock(pending_mtx_);
                pending_acks_.erase(rtsp.req_id);
            }
            (void)publish_cmd_ack(rtsp.req_id, kCmdStartRtspPush, "error",
                                  kCmdRejectReasonEncoderError);
        }
        return;
    }

    if(cmd_name == kCmdStopRtspPush) {
        RtspCommand rtsp{};
        if(!parse_rtsp_stop_command(data_json, req_id, rtsp, reject_reason)) {
            (void)publish_cmd_ack(req_id, kCmdStopRtspPush, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str());
            return;
        }
        if(!track_pending_ack(rtsp.req_id, kCmdStopRtspPush)) {
            (void)publish_cmd_ack(rtsp.req_id, kCmdStopRtspPush, "rejected",
                                  kCmdRejectReasonInvalidState);
            return;
        }
        if(!dispatch_rtsp_stop_to_media(rtsp)) {
            {
                std::lock_guard<std::mutex> lock(pending_mtx_);
                pending_acks_.erase(rtsp.req_id);
            }
            (void)publish_cmd_ack(rtsp.req_id, kCmdStopRtspPush, "error",
                                  kCmdRejectReasonEncoderError);
        }
        return;
    }

    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT RX command ignored req_id=%s cmd=%s",
        req_id.c_str(), cmd_name.c_str());
    (void)publish_cmd_ack(req_id, cmd_name, "rejected",
                          kCmdRejectReasonInvalidState);
}

bool MqttService::parse_common_command(const std::string& payload,
                                       std::string& req_id,
                                       std::string& cmd,
                                       std::string& data_json,
                                       std::string& reject_reason) const
{
    req_id.clear();
    cmd.clear();
    data_json.clear();
    reject_reason.clear();

    std::string type;
    if(json_extract_string(payload, "type", type) && type != "command") {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    if(!json_extract_string(payload, "req_id", req_id)) {
        (void)json_extract_string(payload, "cmdId", req_id);
    }
    if(req_id.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    std::string device_id;
    if(json_extract_string(payload, "device_id", device_id) &&
       device_id != cfg_.device_id) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    if(!json_extract_object(payload, "data", data_json)) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_string(data_json, "cmd", cmd) || cmd.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    return true;
}

bool MqttService::parse_light_command(const std::string& data_json,
                                      const std::string& req_id,
                                      LightCommand& out,
                                      std::string& reject_reason) const
{
    reject_reason.clear();

    std::string zone;
    if(!json_extract_string(data_json, "zone", zone) || zone != "chamber") {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    std::string mode;
    if(!json_extract_string(data_json, "mode", mode) ||
       (mode != "on" && mode != "off")) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    int brightness = -1;
    if(json_extract_int(data_json, "brightness", brightness)) {
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

bool MqttService::parse_rtsp_start_command(const std::string& data_json,
                                           const std::string& req_id,
                                           RtspCommand& out,
                                           std::string& reject_reason) const
{
    reject_reason.clear();
    out = RtspCommand{};
    out.req_id = req_id;

    if(!json_extract_string(data_json, "stream_type", out.stream_type) ||
       out.stream_type.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_string(data_json, "push_url", out.push_url) ||
       out.push_url.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    (void)json_extract_string(data_json, "push_token", out.push_token);
    (void)json_extract_string(data_json, "recording_id", out.recording_id);

    int seek_s = 0;
    if(json_extract_int(data_json, "seek_s", seek_s) && seek_s >= 0) {
        out.seek_s = static_cast<uint32_t>(seek_s);
    }
    int bitrate = 0;
    if(json_extract_int(data_json, "max_bitrate_kbps", bitrate) && bitrate > 0) {
        out.max_bitrate_kbps = static_cast<uint32_t>(bitrate);
    }

    std::string transport;
    if(json_extract_string(data_json, "transport", transport) &&
       !transport.empty() && transport != "tcp") {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    return true;
}

bool MqttService::parse_rtsp_stop_command(const std::string& data_json,
                                          const std::string& req_id,
                                          RtspCommand& out,
                                          std::string& reject_reason) const
{
    reject_reason.clear();
    out = RtspCommand{};
    out.req_id = req_id;
    if(!json_extract_string(data_json, "stream_type", out.stream_type) ||
       out.stream_type.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
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

bool MqttService::dispatch_rtsp_start_to_media(const RtspCommand& cmd)
{
    middleware::RtspPushStartReqPayload p{};
    std::snprintf(p.req_id, sizeof(p.req_id), "%s", cmd.req_id.c_str());
    std::snprintf(p.push_url, sizeof(p.push_url), "%s", cmd.push_url.c_str());
    std::snprintf(p.push_token, sizeof(p.push_token), "%s", cmd.push_token.c_str());
    std::snprintf(p.stream_type, sizeof(p.stream_type), "%s",
                  cmd.stream_type.c_str());
    std::snprintf(p.recording_id, sizeof(p.recording_id), "%s",
                  cmd.recording_id.c_str());
    p.seek_s = cmd.seek_s;
    p.max_bitrate_kbps = cmd.max_bitrate_kbps;
    p.transport = 0;

    middleware::Message msg(middleware::MsgType::MQTT_Media_Rtsp_Start_Req,
        middleware::ModuleId::APP_MQTT,
        middleware::ModuleId::APP_MEDIA);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    const bool ok = middleware::MsgPort::instance().publish_async(msg);
    if(!ok) {
        UPC_LOG_MQTT_SERVICE(Warn, "dispatch start_rtsp_push failed");
    } else {
        UPC_LOG_MQTT_SERVICE(Info,
            "dispatch start_rtsp_push req_id=%s stream_type=%s",
            cmd.req_id.c_str(), cmd.stream_type.c_str());
    }
    return ok;
}

bool MqttService::dispatch_rtsp_stop_to_media(const RtspCommand& cmd)
{
    middleware::RtspPushStopReqPayload p{};
    std::snprintf(p.req_id, sizeof(p.req_id), "%s", cmd.req_id.c_str());
    std::snprintf(p.stream_type, sizeof(p.stream_type), "%s",
                  cmd.stream_type.c_str());

    middleware::Message msg(middleware::MsgType::MQTT_Media_Rtsp_Stop_Req,
        middleware::ModuleId::APP_MQTT,
        middleware::ModuleId::APP_MEDIA);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    const bool ok = middleware::MsgPort::instance().publish_async(msg);
    if(!ok) {
        UPC_LOG_MQTT_SERVICE(Warn, "dispatch stop_rtsp_push failed");
    } else {
        UPC_LOG_MQTT_SERVICE(Info,
            "dispatch stop_rtsp_push req_id=%s stream_type=%s",
            cmd.req_id.c_str(), cmd.stream_type.c_str());
    }
    return ok;
}

bool MqttService::track_pending_ack(const std::string& req_id, const std::string& cmd)
{
    if(req_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(pending_mtx_);
    if(pending_acks_.find(req_id) != pending_acks_.end()) {
        UPC_LOG_MQTT_SERVICE(Warn, "duplicate pending req_id=%s", req_id.c_str());
        return false;
    }
    PendingAck pending{};
    pending.cmd = cmd;
    pending.deadline_ms = monotonic_ms() + kRtspAckTimeoutMs;
    pending_acks_[req_id] = pending;
    return true;
}

void MqttService::expire_pending_acks()
{
    const uint64_t now = monotonic_ms();
    std::vector<std::pair<std::string, std::string>> expired;
    {
        std::lock_guard<std::mutex> lock(pending_mtx_);
        for(auto it = pending_acks_.begin(); it != pending_acks_.end();) {
            if(now >= it->second.deadline_ms) {
                expired.emplace_back(it->first, it->second.cmd);
                it = pending_acks_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for(const auto& item : expired) {
        UPC_LOG_MQTT_SERVICE(Warn,
            "rtsp ack timeout req_id=%s cmd=%s",
            item.first.c_str(), item.second.c_str());
        (void)publish_cmd_ack(item.first, item.second, "error",
                              kCmdRejectReasonEncoderError);
    }
}

void MqttService::on_rtsp_start_rsp(const middleware::Message& msg)
{
    handle_rtsp_rsp(msg, kCmdStartRtspPush);
}

void MqttService::on_rtsp_stop_rsp(const middleware::Message& msg)
{
    handle_rtsp_rsp(msg, kCmdStopRtspPush);
}

void MqttService::handle_rtsp_rsp(const middleware::Message& msg,
                                  const char* expect_cmd)
{
    middleware::RtspPushRspPayload rsp{};
    if(!read_inline_pod(msg.payload, rsp)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid rtsp rsp payload cmd=%s", expect_cmd);
        return;
    }
    rsp.req_id[sizeof(rsp.req_id) - 1] = '\0';
    rsp.reason[sizeof(rsp.reason) - 1] = '\0';
    rsp.stream_type[sizeof(rsp.stream_type) - 1] = '\0';

    {
        std::lock_guard<std::mutex> lock(pending_mtx_);
        auto it = pending_acks_.find(rsp.req_id);
        if(it == pending_acks_.end()) {
            UPC_LOG_MQTT_SERVICE(Warn,
                "rtsp rsp without pending req_id=%s", rsp.req_id);
            return;
        }
        if(it->second.cmd != expect_cmd) {
            UPC_LOG_MQTT_SERVICE(Warn,
                "rtsp rsp cmd mismatch req_id=%s expect=%s got=%s",
                rsp.req_id, expect_cmd, it->second.cmd.c_str());
            return;
        }
        pending_acks_.erase(it);
    }

    const char* result = "error";
    const char* reason = nullptr;
    char detail[128];
    std::snprintf(detail, sizeof(detail), "{}");
    switch(static_cast<middleware::RtspPushResult>(rsp.result)) {
    case middleware::RtspPushResult::Accepted:
        result = "accepted";
        if(rsp.push_started_at > 0) {
            std::snprintf(detail, sizeof(detail),
                          "{\"push_started_at\":%llu}",
                          static_cast<unsigned long long>(rsp.push_started_at));
        }
        break;
    case middleware::RtspPushResult::Rejected:
        result = "rejected";
        reason = rsp.reason[0] ? rsp.reason : kCmdRejectReasonInvalidState;
        break;
    case middleware::RtspPushResult::Error:
    default:
        result = "error";
        reason = rsp.reason[0] ? rsp.reason : kCmdRejectReasonEncoderError;
        break;
    }
    (void)publish_cmd_ack(rsp.req_id, expect_cmd, result, reason, detail);
}

int MqttService::on_message_arrived(void* context, char* topic_name,
                                    int topic_len, MQTTClient_message* message)
{
    if(!message) {
        if(topic_name) {
            MQTTClient_free(topic_name);
        }
        return 1;
    }

    std::string topic;
    if(topic_name) {
        if(topic_len > 0) {
            topic.assign(topic_name, static_cast<size_t>(topic_len));
        } else {
            topic.assign(topic_name);
        }
    }

    /* 原始帧必须先于任何格式校验打印；空载荷/非法 JSON 同样可在日志中看到。 */
    const size_t payload_len = message->payloadlen > 0
        ? static_cast<size_t>(message->payloadlen)
        : 0U;
#if UPC_MQTT_RX_RAW_LOG_ENABLE
    debug_log_mqtt_frame("RX", topic, message->payload,
                         payload_len,
                         message->qos, message->retained);
#endif

    if(context) {
        auto* self = static_cast<MqttService*>(context);
        const std::string payload = (message->payload && payload_len > 0)
            ? std::string(static_cast<const char*>(message->payload), payload_len)
            : std::string{};
        self->handle_command_message(payload);
    } else {
        UPC_LOG_MQTT_SERVICE(Warn,
            "MQTT RX frame dropped because callback context is null");
    }

    MQTTClient_freeMessage(&message);
    if(topic_name) {
        MQTTClient_free(topic_name);
    }
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

std::string MqttService::read_boot_id()
{
    std::ifstream in("/proc/sys/kernel/random/boot_id");
    std::string boot_id;
    if(std::getline(in, boot_id) && !boot_id.empty()) {
        return boot_id;
    }
    const long long now = static_cast<long long>(unix_ms());
    return std::string("boot-") + std::to_string(now);
}

int64_t MqttService::read_uptime_s()
{
    std::ifstream in("/proc/uptime");
    double uptime = 0.0;
    if(in >> uptime && uptime >= 0.0) {
        return static_cast<int64_t>(uptime);
    }
    return 0;
}

int MqttService::bars_to_rssi_dbm(int bars, bool connected)
{
    if(!connected) {
        return -127;
    }
    if(bars >= 3) {
        return -55;
    }
    if(bars == 2) {
        return -67;
    }
    if(bars == 1) {
        return -78;
    }
    return -90;
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

bool MqttService::json_extract_object(const std::string& json, const char* key,
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
    if(pos >= json.size() || json[pos] != '{') {
        return false;
    }

    const size_t object_begin = pos;
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for(; pos < json.size(); ++pos) {
        const char c = json[pos];
        if(in_string) {
            if(escaped) {
                escaped = false;
            } else if(c == '\\') {
                escaped = true;
            } else if(c == '"') {
                in_string = false;
            }
            continue;
        }

        if(c == '"') {
            in_string = true;
        } else if(c == '{') {
            ++depth;
        } else if(c == '}') {
            if(depth == 0) {
                return false;
            }
            --depth;
            if(depth == 0) {
                out.assign(json, object_begin, pos - object_begin + 1);
                return true;
            }
        }
    }
    return false;
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
