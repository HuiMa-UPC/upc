#include "mqtt_service.hpp"

#include "mqtt_service_internal.hpp"
#include "rs_log.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace upc::mqtt {

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

void MqttService::on_mcu_version(const middleware::Message& msg)
{
    middleware::McuVersionPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid GLAVO_Mcu_Version_Ntf payload");
        return;
    }
    bool publish_now = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        fw_cache_.mcu = p;
        fw_cache_.mcu_valid = (p.valid != 0);
        if(ota_hb_wait_.active && ota_hb_wait_.need_mcu) {
            ota_hb_wait_.got_mcu = true;
            if((!ota_hb_wait_.need_mcu || ota_hb_wait_.got_mcu) &&
               (!ota_hb_wait_.need_galvo || ota_hb_wait_.got_galvo)) {
                ota_hb_wait_.active = false;
                publish_now = true;
            }
        }
    }
    if(publish_now) {
        last_heartbeat_pub_ms_ = 0;
        (void)publish_heartbeat();
    }
}

void MqttService::on_galvo_version(const middleware::Message& msg)
{
    middleware::GalvoVersionPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid GLAVO_Galvo_Version_Ntf payload");
        return;
    }
    bool publish_now = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        fw_cache_.galvo = p;
        fw_cache_.galvo_valid = (p.valid != 0);
        if(ota_hb_wait_.active && ota_hb_wait_.need_galvo) {
            ota_hb_wait_.got_galvo = true;
            if((!ota_hb_wait_.need_mcu || ota_hb_wait_.got_mcu) &&
               (!ota_hb_wait_.need_galvo || ota_hb_wait_.got_galvo)) {
                ota_hb_wait_.active = false;
                publish_now = true;
            }
        }
    }
    if(publish_now) {
        last_heartbeat_pub_ms_ = 0;
        (void)publish_heartbeat();
    }
}

void MqttService::on_env_sim_state(const middleware::Message& msg)
{
    middleware::DebugOnOffPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid GLAVO_Debug_Env_Sim_Ntf payload");
        return;
    }
    std::lock_guard<std::mutex> lock(data_mtx_);
    env_sim_on_ = (p.on != 0);
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

void MqttService::request_firmware_versions_if_needed()
{
    bool need_mcu = false;
    bool need_galvo = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        need_mcu = !fw_cache_.mcu_valid;
        need_galvo = !fw_cache_.galvo_valid;
    }
    if(!need_mcu && !need_galvo) {
        return;
    }

    const uint64_t now = monotonic_ms();
    if(last_fw_query_ms_ != 0 && now - last_fw_query_ms_ < kFirmwareQueryIntervalMs) {
        return;
    }
    last_fw_query_ms_ = now;

    auto& port = middleware::MsgPort::instance();
    if(need_mcu) {
        middleware::Message req(middleware::MsgType::UI_Mcu_Version_Ntf,
                                middleware::ModuleId::APP_MQTT,
                                middleware::ModuleId::APP_GLAVO);
        (void)port.publish_async(req);
    }
    if(need_galvo) {
        middleware::Message req(middleware::MsgType::UI_Galvo_Version_Ntf,
                                middleware::ModuleId::APP_MQTT,
                                middleware::ModuleId::APP_GLAVO);
        (void)port.publish_async(req);
    }
}

void MqttService::request_firmware_versions_force(bool need_mcu, bool need_galvo)
{
    auto& port = middleware::MsgPort::instance();
    if(need_mcu) {
        middleware::Message req(middleware::MsgType::UI_Mcu_Version_Ntf,
                                middleware::ModuleId::APP_MQTT,
                                middleware::ModuleId::APP_GLAVO);
        (void)port.publish_async(req);
    }
    if(need_galvo) {
        middleware::Message req(middleware::MsgType::UI_Galvo_Version_Ntf,
                                middleware::ModuleId::APP_MQTT,
                                middleware::ModuleId::APP_GLAVO);
        (void)port.publish_async(req);
    }
}

void MqttService::poll_ota_heartbeat_wait()
{
    bool timeout_publish = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        if(ota_hb_wait_.active && monotonic_ms() >= ota_hb_wait_.deadline_ms) {
            ota_hb_wait_.active = false;
            timeout_publish = true;
        }
    }
    if(timeout_publish) {
        last_heartbeat_pub_ms_ = 0;
        (void)publish_heartbeat();
    }
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
    middleware::McuVersionPayload mcu_ver{};
    middleware::GalvoVersionPayload galvo_ver{};
    bool mcu_valid = false;
    bool galvo_valid = false;
    bool env_sim_on = false;
    std::string soc_override;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        wifi_connected = latest_.wifi_connected;
        signal_bars = latest_.signal_bars;
        mcu_ver = fw_cache_.mcu;
        galvo_ver = fw_cache_.galvo;
        mcu_valid = fw_cache_.mcu_valid;
        galvo_valid = fw_cache_.galvo_valid;
        env_sim_on = env_sim_on_;
        soc_override = soc_fw_override_;
    }

    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const long long uptime_s = read_uptime_s();
    const int rssi = bars_to_rssi_dbm(signal_bars, wifi_connected);
    const char* network_type = wifi_connected ? "wifi" : "none";
    const std::string escaped_device = json_escape(cfg_.device_id);
    const std::string escaped_boot = json_escape(boot_id_);
    const std::string soc_fw = (env_sim_on && !soc_override.empty())
        ? soc_override
        : std::string(UPC_SW_VERSION);
    const std::string escaped_soc_fw = json_escape(soc_fw);
    const std::string escaped_mcu_fw = json_escape(
        mcu_valid ? format_mcu_version(mcu_ver) : std::string("unknown"));
    const std::string escaped_galvo_fw = json_escape(
        galvo_valid ? format_galvo_version(galvo_ver.fw_version_le) : std::string("unknown"));

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
            "\"firmwares\":["
                "{\"firmware_type\":\"soc\",\"version\":\"%s\"},"
                "{\"firmware_type\":\"mcu\",\"version\":\"%s\"},"
                "{\"firmware_type\":\"galvo\",\"version\":\"%s\"}"
            "],"
            "\"battery\":null,"
            "\"network\":{\"type\":\"%s\",\"rssi\":%d}"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        escaped_boot.c_str(),
        uptime_s,
        escaped_soc_fw.c_str(),
        escaped_mcu_fw.c_str(),
        escaped_galvo_fw.c_str(),
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

} // namespace upc::mqtt
