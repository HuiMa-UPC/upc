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
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn, "invalid APP_Update_Ntf payload");
        return;
    }
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Debug,
        "IPC RX APP_Update_Ntf status=%u pm25=%.3f o2=%.3f%% temp=%.3fC "
        "humidity=%.3f%% pressure=%.3fkPa",
        static_cast<unsigned>(p.status),
        static_cast<double>(p.pm25_cabin),
        static_cast<double>(p.oxygen_percent),
        static_cast<double>(p.temperature_c),
        static_cast<double>(p.humidity_percent),
        static_cast<double>(p.pressure_kpa));
    cache_telemetry(p);
    if(p.status == static_cast<uint32_t>(middleware::UserHomeStatus::Ready) ||
       p.status == static_cast<uint32_t>(middleware::UserHomeStatus::Idle)) {
        note_tcp_ready(p.status == static_cast<uint32_t>(middleware::UserHomeStatus::Ready));
    }
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

void MqttService::on_cloud_state(const middleware::Message& msg)
{
    middleware::CloudStateNtfPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttState, Warn, "invalid GLAVO_Cloud_State_Ntf payload");
        return;
    }
    p.mode[sizeof(p.mode) - 1] = '\0';
    p.substate[sizeof(p.substate) - 1] = '\0';
    p.job_id[sizeof(p.job_id) - 1] = '\0';
    UPC_LOG_MQTT_SERVICE_TO(MqttState, Debug,
        "IPC RX GLAVO_Cloud_State_Ntf %s/%s layer=%u pct=%d",
        p.mode, p.substate, p.layer, p.progress_pct);
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        std::snprintf(device_cloud_.mode, sizeof(device_cloud_.mode), "%s", p.mode);
        std::snprintf(device_cloud_.substate, sizeof(device_cloud_.substate), "%s", p.substate);
        device_cloud_.job_valid = p.job_valid != 0;
        device_cloud_.layer = p.layer;
        device_cloud_.total_layers = p.total_layers;
        device_cloud_.progress_pct = p.progress_pct;
        device_cloud_.eta_s = p.eta_s;
        device_cloud_.since_ms = p.since_ms;
        device_cloud_.started_at_ms = p.started_at_ms;
        std::snprintf(device_cloud_.job_id, sizeof(device_cloud_.job_id), "%s", p.job_id);
        if(std::strcmp(p.mode, "idle") == 0) {
            tcp_ready_known_ = true;
            tcp_ready_ = std::strcmp(p.substate, "ready") == 0;
        }
    }
    (void)publish_cloud_state(false);
}

void MqttService::note_tcp_ready(bool ready)
{
    bool publish = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        if(tcp_ready_known_ && tcp_ready_ == ready) {
            return;
        }
        tcp_ready_known_ = true;
        tcp_ready_ = ready;
        publish = !ota_overlay_ && std::strcmp(device_cloud_.mode, "idle") == 0;
    }
    if(publish) {
        (void)publish_cloud_state(false);
    }
}

void MqttService::note_ota_cloud_state(const char* ota_state)
{
    const char* sub = (ota_state && ota_state[0] != '\0') ? ota_state : "downloading";
    if(std::strcmp(sub, "rolled_back") == 0) {
        sub = "failed";
    }
    const bool terminal =
        std::strcmp(sub, "completed") == 0 || std::strcmp(sub, "failed") == 0;
    bool publish_overlay = false;
    bool publish_device = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        if(std::strcmp(ota_sub_, sub) == 0 && (ota_overlay_ || terminal)) {
            return;
        }
        if(!ota_overlay_) {
            ota_since_ms_ = static_cast<uint64_t>(unix_ms());
        }
        ota_overlay_ = true;
        std::snprintf(ota_sub_, sizeof(ota_sub_), "%s", sub);
        publish_overlay = true;
        publish_device = terminal;
    }
    if(publish_overlay) {
        (void)publish_cloud_state(false);
    }
    if(publish_device) {
        {
            std::lock_guard<std::mutex> lock(data_mtx_);
            ota_overlay_ = false;
        }
        (void)publish_cloud_state(true);
    }
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
    /* chamber 用舱内量；舱外 PM 另走 chamber_ext */
    latest_.pm25 = p.pm25_cabin;
    latest_.pm1_0 = p.pm1_cabin;
    latest_.pm10 = p.pm10_cabin;
    latest_.pm25_outdoor = p.pm25_indoor;
    latest_.oxygen_percent = p.oxygen_percent;
    latest_.temperature_c = p.temperature_c;
    latest_.humidity_percent = p.humidity_percent;
    latest_.pressure_kpa = p.pressure_kpa;
    latest_.baseplate_temp_c = p.baseplate_temp_c;
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

    auto json_num_or_null = [](float value, const char* format) -> std::string {
        if(value < 0.0f) {
            return "null";
        }
        char num[64];
        std::snprintf(num, sizeof(num), format, static_cast<double>(value));
        return std::string(num);
    };
    /* 无效氧发 null，禁止夹成 0 ppm（云端会误判为已洗气到位） */
    std::string o2_ppm = "null";
    if(snap.oxygen_percent >= 0.0f) {
        const int ppm = static_cast<int>(
            std::lround(static_cast<double>(snap.oxygen_percent) * 10000.0));
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", ppm < 0 ? 0 : ppm);
        o2_ppm = buf;
    }
    const std::string pm25 = json_num_or_null(snap.pm25, "%.3f");
    const std::string pm1_0 = json_num_or_null(snap.pm1_0, "%.3f");
    const std::string pm10 = json_num_or_null(snap.pm10, "%.3f");
    const std::string humidity = json_num_or_null(snap.humidity_percent, "%.3f");
    const std::string pressure_1 = json_num_or_null(snap.pressure_kpa, "%.3f");
    const std::string ntc_c = json_num_or_null(snap.temperature_c, "%.3f");
    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const std::string escaped_device = json_escape(cfg_.device_id);

    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"telemetry\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"data\":{"
            "\"group\":\"chamber\","
            "\"pm25\":%s,"
            "\"pm1_0\":%s,"
            "\"pm10\":%s,"
            "\"humidity\":%s,"
            "\"pressure_1\":%s,"
            "\"o2_ppm\":%s,"
            "\"ntc_c\":%s"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        pm25.c_str(),
        pm1_0.c_str(),
        pm10.c_str(),
        humidity.c_str(),
        pressure_1.c_str(),
        o2_ppm.c_str(),
        ntc_c.c_str());

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
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn, "publish telemetry fail rc=%d topic=%s",
            rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Debug,
        "MQTT TX telemetry queued topic=%s token=%d",
        topic.c_str(), static_cast<int>(token));
    return true;
}

bool MqttService::publish_chamber_ext_telemetry()
{
    ChamberTelemetry snap{};
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        snap = latest_;
    }
    /* 舱外 PM 走 chamber_ext；无数则不发 */
    if(!snap.has_value || snap.pm25_outdoor < 0.0f) {
        return false;
    }

    const uint64_t now = monotonic_ms();
    if(last_chamber_ext_pub_ms_ != 0 &&
       now - last_chamber_ext_pub_ms_ < kTelemetryChamberExtIntervalMs) {
        return false;
    }
    last_chamber_ext_pub_ms_ = now;

    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const std::string escaped_device = json_escape(cfg_.device_id);

    char payload[512];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"telemetry\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"data\":{"
            "\"group\":\"chamber_ext\","
            "\"pm25\":%.3f"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        static_cast<double>(snap.pm25_outdoor));

    const std::string topic = cfg_.topic_telemetry_chamber_ext();
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
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "publish chamber_ext fail rc=%d topic=%s", rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Debug,
        "MQTT TX chamber_ext queued topic=%s token=%d",
        topic.c_str(), static_cast<int>(token));
    return true;
}

bool MqttService::publish_cloud_state(bool force)
{
    /* updating 盖住 Glavo 的 idle/printing。idle 的 ready/not_ready 跟振镜 TCP。
     * health 仍是占位 ok。job 只在 printing 时带；eta 未知写 null。 */
    char mode[16];
    char substate[32];
    bool job_valid = false;
    uint32_t layer = 0;
    uint32_t total_layers = 0;
    int32_t progress_pct = 0;
    int32_t eta_s = -1;
    uint64_t since_ms = 0;
    uint64_t started_at_ms = 0;
    char job_id[64];
    job_id[0] = '\0';

    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        if(ota_overlay_) {
            std::snprintf(mode, sizeof(mode), "updating");
            std::snprintf(substate, sizeof(substate), "%s",
                          ota_sub_[0] ? ota_sub_ : "downloading");
            since_ms = ota_since_ms_;
        } else {
            std::snprintf(mode, sizeof(mode), "%s", device_cloud_.mode);
            std::snprintf(substate, sizeof(substate), "%s", device_cloud_.substate);
            if(std::strcmp(mode, "idle") == 0 && tcp_ready_known_) {
                std::snprintf(substate, sizeof(substate), "%s",
                              tcp_ready_ ? "ready" : "not_ready");
            }
            job_valid = device_cloud_.job_valid && std::strcmp(mode, "printing") == 0;
            layer = device_cloud_.layer;
            total_layers = device_cloud_.total_layers;
            progress_pct = device_cloud_.progress_pct;
            eta_s = device_cloud_.eta_s;
            since_ms = device_cloud_.since_ms;
            started_at_ms = device_cloud_.started_at_ms;
            std::snprintf(job_id, sizeof(job_id), "%s", device_cloud_.job_id);
        }
        if(since_ms == 0) {
            since_ms = static_cast<uint64_t>(unix_ms());
            if(!ota_overlay_ && std::strcmp(mode, "idle") == 0) {
                device_cloud_.since_ms = since_ms;
            }
        }
        char fp[192];
        std::snprintf(fp, sizeof(fp), "%s|%s|%u|%d|%u|%s",
                      mode, substate, layer, progress_pct,
                      job_valid ? 1u : 0u, job_valid ? job_id : "");
        if(!force && std::strcmp(cloud_pub_fp_, fp) == 0) {
            return true;
        }
        std::snprintf(cloud_pub_fp_, sizeof(cloud_pub_fp_), "%s", fp);
    }

    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const std::string escaped_device = json_escape(cfg_.device_id);

    char job_json[512];
    if(!job_valid) {
        std::snprintf(job_json, sizeof(job_json), "\"job\":null");
    } else if(eta_s < 0) {
        std::snprintf(job_json, sizeof(job_json),
            "\"job\":{"
            "\"job_id\":\"%s\","
            "\"layer\":%u,"
            "\"total_layers\":%u,"
            "\"progress_pct\":%d,"
            "\"eta_s\":null,"
            "\"started_at\":%llu"
            "}",
            json_escape(job_id).c_str(),
            layer,
            total_layers,
            progress_pct,
            static_cast<unsigned long long>(started_at_ms));
    } else {
        std::snprintf(job_json, sizeof(job_json),
            "\"job\":{"
            "\"job_id\":\"%s\","
            "\"layer\":%u,"
            "\"total_layers\":%u,"
            "\"progress_pct\":%d,"
            "\"eta_s\":%d,"
            "\"started_at\":%llu"
            "}",
            json_escape(job_id).c_str(),
            layer,
            total_layers,
            progress_pct,
            eta_s,
            static_cast<unsigned long long>(started_at_ms));
    }

    char payload[1280];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"state\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"data\":{"
            "\"mode\":\"%s\","
            "\"substate\":\"%s\","
            "\"since\":%lld,"
            "%s,"
            "\"health\":{"
                "\"laser\":\"ok\","
                "\"galvo\":\"ok\","
                "\"motion\":\"ok\","
                "\"dust\":\"ok\","
                "\"safety\":\"ok\""
            "}"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        mode,
        substate,
        static_cast<long long>(since_ms),
        job_json);

    const std::string topic = cfg_.topic_state();
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
        UPC_LOG_MQTT_SERVICE_TO(MqttState, Warn, "publish state fail rc=%d topic=%s",
                             rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        {
            std::lock_guard<std::mutex> lock(data_mtx_);
            cloud_pub_fp_[0] = '\0';
        }
        return false;
    }
    UPC_LOG_MQTT_SERVICE_TO(MqttState, Debug,
        "MQTT TX state queued topic=%s token=%d",
        topic.c_str(), static_cast<int>(token));
    return true;
}

bool MqttService::publish_heartbeat(bool force)
{
    const uint64_t now = monotonic_ms();
    const uint64_t interval_ms =
        static_cast<uint64_t>(cfg_.heartbeat_interval_sec) * 1000ULL;
    if(!force && last_heartbeat_pub_ms_ != 0 &&
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
    const std::string soc_fw_raw = (env_sim_on && !soc_override.empty())
        ? soc_override
        : std::string(UPC_SW_VERSION);
    const std::string soc_fw = data_plane_version(soc_fw_raw);
    const std::string escaped_soc_fw = json_escape(soc_fw.empty() ? "unknown" : soc_fw);
    /* 整机版本是 check-update 的对账锚，只上报纯数字点分；没有合法版本时发 null。 */
    const std::string release_ver = numeric_release_version(UPC_SW_VERSION);
    const std::string release_json = release_ver.empty()
        ? std::string("null")
        : ("\"" + json_escape(release_ver) + "\"");
    const std::string escaped_mcu_fw = json_escape(
        mcu_valid ? format_mcu_version(mcu_ver) : std::string("unknown"));
    const std::string escaped_galvo_fw = json_escape(
        galvo_valid ? format_galvo_version(galvo_ver.fw_version_le) : std::string("unknown"));

    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":3,"
        "\"type\":\"heartbeat\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"boot_id\":\"%s\","
        "\"data\":{"
            "\"uptime_s\":%lld,"
            "\"release_version\":%s,"
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
        release_json.c_str(),
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
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn, "publish heartbeat fail rc=%d topic=%s",
                             rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }

    last_heartbeat_pub_ms_ = now;
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Debug,
        "MQTT TX heartbeat queued topic=%s token=%d",
        topic.c_str(), static_cast<int>(token));
    return true;
}

} // namespace upc::mqtt
