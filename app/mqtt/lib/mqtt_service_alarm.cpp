#include "mqtt_service.hpp"

#include "mqtt_service_internal.hpp"
#include "rs_log.hpp"

#include <MQTTClient.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace upc::mqtt {

namespace {

std::string read_uuid_file()
{
    std::ifstream in("/proc/sys/kernel/random/uuid");
    std::string id;
    if(std::getline(in, id)) {
        while(!id.empty() &&
              (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) {
            id.pop_back();
        }
        if(!id.empty()) {
            return id;
        }
    }
    return {};
}

std::string mcu_link_text(uint8_t link)
{
    switch(static_cast<middleware::AlarmMcuLink>(link)) {
    case middleware::AlarmMcuLink::Online:
        return "online";
    case middleware::AlarmMcuLink::Offline:
        return "offline";
    case middleware::AlarmMcuLink::Unknown:
    default:
        return "unknown";
    }
}

} // namespace

std::string MqttService::make_uuid_v4()
{
    std::string id = read_uuid_file();
    if(!id.empty()) {
        return id;
    }
    /* /proc 不可用时退化为时间戳伪 ID（仅开发机兜底，板上应有 random/uuid）。 */
    const long long now = static_cast<long long>(unix_ms());
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "00000000-0000-4000-8000-%012llx",
                  static_cast<unsigned long long>(now & 0xffffffffffffLL));
    return std::string(buf);
}

void MqttService::alarm_stream_load()
{
    std::ifstream in(kAlarmStreamFile);
    if(!in) {
        alarm_stream_id_ = make_uuid_v4();
        alarm_report_seq_ = 0;
        alarm_stream_save();
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Info,
            "alarm stream created id=%s", alarm_stream_id_.c_str());
        return;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    std::string sid;
    int64_t seq = 0;
    if(json_extract_string(content, "stream_id", sid) && !sid.empty()) {
        alarm_stream_id_ = sid;
    } else {
        alarm_stream_id_ = make_uuid_v4();
    }
    if(!json_extract_int64(content, "report_seq", seq) || seq < 0) {
        seq = 0;
    }
    alarm_report_seq_ = seq;
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Info,
        "alarm stream loaded id=%s report_seq=%lld",
        alarm_stream_id_.c_str(),
        static_cast<long long>(alarm_report_seq_));
}

void MqttService::alarm_stream_save()
{
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(kAlarmStreamFile).parent_path(), ec);
    std::ofstream out(kAlarmStreamFile, std::ios::trunc);
    if(!out) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "alarm stream save failed path=%s", kAlarmStreamFile);
        return;
    }
    out << "{\"stream_id\":\"" << json_escape(alarm_stream_id_)
        << "\",\"report_seq\":" << alarm_report_seq_ << "}\n";
}

void MqttService::alarm_outbox_load()
{
    alarm_outbox_.clear();
    std::error_code ec;
    const std::filesystem::path dir(kAlarmOutboxDir);
    if(!std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        return;
    }
    std::vector<std::filesystem::path> files;
    for(const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
        if(!ent.is_regular_file(ec)) {
            continue;
        }
        if(ent.path().extension() != ".json") {
            continue;
        }
        files.push_back(ent.path());
    }
    std::sort(files.begin(), files.end());
    for(const auto& path : files) {
        std::ifstream in(path);
        if(!in) {
            continue;
        }
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        if(body.empty()) {
            continue;
        }
        AlarmOutboxItem item;
        item.message_id = path.stem().string();
        (void)json_extract_string(body, "message_id", item.message_id);
        (void)json_extract_string(body, "report_seq", item.report_seq);
        std::string type;
        (void)json_extract_string(body, "type", type);
        item.is_snapshot = (type == "alarm.snapshot");
        item.body = std::move(body);
        alarm_outbox_.push_back(std::move(item));
    }
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Info,
        "alarm outbox loaded count=%zu", alarm_outbox_.size());
}

void MqttService::alarm_outbox_trim()
{
    if(alarm_outbox_.size() <= kAlarmOutboxMaxItems) {
        return;
    }
    /* 超限：丢掉最旧的非快照；永远保留最新一份快照。 */
    while(alarm_outbox_.size() > kAlarmOutboxMaxItems) {
        size_t drop = static_cast<size_t>(-1);
        for(size_t i = 0; i < alarm_outbox_.size(); ++i) {
            if(!alarm_outbox_[i].is_snapshot) {
                drop = i;
                break;
            }
        }
        if(drop == static_cast<size_t>(-1)) {
            /* 全是快照：只留最后一条 */
            while(alarm_outbox_.size() > 1) {
                alarm_outbox_erase(alarm_outbox_.front().message_id);
                alarm_outbox_.erase(alarm_outbox_.begin());
            }
            break;
        }
        alarm_outbox_erase(alarm_outbox_[drop].message_id);
        alarm_outbox_.erase(alarm_outbox_.begin() +
                            static_cast<std::ptrdiff_t>(drop));
    }
}

bool MqttService::alarm_outbox_persist(const std::string& message_id,
                                       const std::string& body,
                                       bool /*is_snapshot*/)
{
    std::error_code ec;
    std::filesystem::create_directories(kAlarmOutboxDir, ec);
    const std::filesystem::path path =
        std::filesystem::path(kAlarmOutboxDir) / (message_id + ".json");
    std::ofstream out(path, std::ios::trunc);
    if(!out) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "alarm outbox write failed id=%s", message_id.c_str());
        return false;
    }
    out << body;
    return static_cast<bool>(out);
}

void MqttService::alarm_outbox_erase(const std::string& message_id)
{
    std::error_code ec;
    const std::filesystem::path path =
        std::filesystem::path(kAlarmOutboxDir) / (message_id + ".json");
    std::filesystem::remove(path, ec);
}

std::string MqttService::alarm_next_message_id()
{
    return make_uuid_v4();
}

std::string MqttService::alarm_alloc_report_seq()
{
    ++alarm_report_seq_;
    if(alarm_report_seq_ < 1) {
        alarm_report_seq_ = 1;
    }
    alarm_stream_save();
    return std::to_string(alarm_report_seq_);
}

std::string MqttService::build_alarm_single_json(
    const middleware::AlarmReportPayload& p,
    const std::string& message_id,
    const std::string& report_seq)
{
    std::ostringstream os;
    os << '{'
       << "\"v\":1,"
       << "\"type\":\"alarm.single\","
       << "\"device_id\":\"" << json_escape(cfg_.device_id) << "\","
       << "\"message_id\":\"" << json_escape(message_id) << "\","
       << "\"stream_id\":\"" << json_escape(alarm_stream_id_) << "\","
       << "\"report_seq\":\"" << report_seq << "\","
       << "\"ts\":0,"
       << "\"data\":{"
       << "\"source_component\":\"main_mcu\","
       << "\"catalog_key\":\"main-mcu\","
       << "\"mcu_boot_id\":null,"
       << "\"alarm_definition_version\":\"" << kAlarmDefinitionVersion << "\","
       << "\"source_seq\":null,"
       << "\"observed_at\":null,"
       << "\"time_quality\":\"unsynced\","
       << "\"alarm_code\":" << static_cast<unsigned>(p.alarm_code) << ','
       << "\"severity\":" << static_cast<unsigned>(p.severity) << ','
       << "\"state_mask\":" << static_cast<unsigned>(p.state_mask) << ','
       << "\"clear_mode\":" << static_cast<unsigned>(p.clear_mode) << ','
       << "\"detail\":" << static_cast<unsigned long long>(p.detail)
       << "}}";
    return os.str();
}

std::string MqttService::build_alarm_snapshot_json(
    const middleware::AlarmCloudSnapshotPayload& p,
    const std::string& message_id,
    const std::string& report_seq)
{
    std::ostringstream entries;
    int reported = 0;
    bool first = true;
    for(uint8_t slot = 0; slot < middleware::kAlarmSnapshotSlotCount; ++slot) {
        const uint32_t active = p.active_mask[slot];
        const uint32_t latched = p.latched_mask[slot];
        const uint32_t any = active | latched;
        if(any == 0) {
            continue;
        }
        for(uint8_t bit = 0; bit < 32; ++bit) {
            const uint32_t m = (1u << bit);
            if((any & m) == 0) {
                continue;
            }
            const uint16_t code = middleware::alarm_snapshot_code_of(slot, bit);
            if(!middleware::alarm_snapshot_covers(code)) {
                continue;
            }
            uint8_t state_mask = 0;
            if(active & m) {
                state_mask |= middleware::kAlarmStateActive;
            }
            if(latched & m) {
                state_mask |= middleware::kAlarmStateLatched;
            }
            if(!first) {
                entries << ',';
            }
            first = false;
            entries << '{'
                    << "\"alarm_code\":" << static_cast<unsigned>(code) << ','
                    << "\"state_mask\":" << static_cast<unsigned>(state_mask) << ','
                    << "\"severity\":null,"
                    << "\"clear_mode\":null,"
                    << "\"detail\":null,"
                    << "\"metadata_origin\":\"unknown\","
                    << "\"metadata_source_seq\":null"
                    << '}';
            ++reported;
        }
    }

    std::ostringstream os;
    os << '{'
       << "\"v\":1,"
       << "\"type\":\"alarm.snapshot\","
       << "\"device_id\":\"" << json_escape(cfg_.device_id) << "\","
       << "\"message_id\":\"" << json_escape(message_id) << "\","
       << "\"stream_id\":\"" << json_escape(alarm_stream_id_) << "\","
       << "\"report_seq\":\"" << report_seq << "\","
       << "\"ts\":0,"
       << "\"data\":{"
       << "\"source_component\":\"main_mcu\","
       << "\"catalog_key\":\"main-mcu\","
       << "\"mcu_boot_id\":null,"
       << "\"alarm_definition_version\":\"" << kAlarmDefinitionVersion << "\","
       << "\"source_seq\":null,"
       << "\"observed_at\":null,"
       << "\"time_quality\":\"unsynced\","
       << "\"coverage\":\"condition_slots_v1\","
       << "\"complete\":true,"
       << "\"reported_count\":" << reported << ','
       << "\"highest_severity\":" << static_cast<unsigned>(p.highest_severity) << ','
       << "\"entries\":[" << entries.str() << "]"
       << "}}";
    return os.str();
}

std::string MqttService::build_alarm_status_json(const std::string& message_id,
                                                 const std::string& report_seq)
{
    std::ostringstream os;
    os << '{'
       << "\"v\":1,"
       << "\"type\":\"alarm.status\","
       << "\"device_id\":\"" << json_escape(cfg_.device_id) << "\","
       << "\"message_id\":\"" << json_escape(message_id) << "\","
       << "\"stream_id\":\"" << json_escape(alarm_stream_id_) << "\","
       << "\"report_seq\":\"" << report_seq << "\","
       << "\"ts\":0,"
       << "\"data\":{"
       << "\"source_component\":\"main_mcu\","
       << "\"catalog_key\":\"main-mcu\","
       << "\"mcu_boot_id\":null,"
       << "\"alarm_definition_version\":\"" << kAlarmDefinitionVersion << "\","
       << "\"mcu_link\":\"" << mcu_link_text(alarm_mcu_link_) << "\","
       << "\"last_source_seq\":null,"
       << "\"capabilities\":{"
       << "\"source_identity\":false,"
       << "\"atomic_snapshot_watermark\":false,"
       << "\"snapshot_metadata\":false"
       << "},"
       << "\"lost_report_ranges\":[],"
       << "\"lost_source_ranges\":[]"
       << "}}";
    return os.str();
}

bool MqttService::alarm_publish_body(const std::string& body)
{
    if(!mqtt_connected_ || !MQTTClient_isConnected(client_)) {
        return false;
    }
    const std::string topic = cfg_.topic_alarm_report();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = const_cast<char*>(body.data());
    msg.payloadlen = static_cast<int>(body.size());
    msg.qos = 1;
    msg.retained = 0;

    debug_log_mqtt_frame("TX", topic, msg.payload,
                         static_cast<size_t>(msg.payloadlen),
                         msg.qos, msg.retained);

    MQTTClient_deliveryToken token;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "alarm/report publish fail rc=%d", rc);
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        alarm_ack_subscribed_ = false;
        return false;
    }
    return true;
}

void MqttService::alarm_enqueue_and_try_send(std::string body, bool is_snapshot)
{
    std::string message_id;
    std::string report_seq;
    (void)json_extract_string(body, "message_id", message_id);
    (void)json_extract_string(body, "report_seq", report_seq);
    if(message_id.empty()) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "alarm enqueue skipped: missing message_id");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(alarm_mtx_);
        if(is_snapshot) {
            /* 新快照替换未 ack 的旧快照，避免 outbox 堆满过期全量。 */
            for(auto it = alarm_outbox_.begin(); it != alarm_outbox_.end();) {
                if(it->is_snapshot) {
                    alarm_outbox_erase(it->message_id);
                    it = alarm_outbox_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        AlarmOutboxItem item;
        item.message_id = message_id;
        item.report_seq = report_seq;
        item.body = body;
        item.is_snapshot = is_snapshot;
        (void)alarm_outbox_persist(message_id, body, is_snapshot);
        alarm_outbox_.push_back(std::move(item));
        alarm_outbox_trim();
    }

    if(alarm_publish_body(body)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Info,
            "alarm TX type=%s message_id=%s report_seq=%s",
            is_snapshot ? "snapshot" : "single/status",
            message_id.c_str(), report_seq.c_str());
    }
}

void MqttService::alarm_flush_outbox()
{
    if(!mqtt_connected_ || !MQTTClient_isConnected(client_)) {
        return;
    }
    std::vector<std::string> bodies;
    {
        std::lock_guard<std::mutex> lock(alarm_mtx_);
        bodies.reserve(alarm_outbox_.size());
        for(const auto& item : alarm_outbox_) {
            bodies.push_back(item.body);
        }
    }
    for(const auto& body : bodies) {
        if(!alarm_publish_body(body)) {
            break;
        }
    }
}

void MqttService::alarm_maybe_publish_status(bool force)
{
    const uint64_t now = monotonic_ms();
    if(!force && last_alarm_status_ms_ != 0 &&
       now - last_alarm_status_ms_ < kAlarmStatusIntervalMs) {
        return;
    }
    last_alarm_status_ms_ = now;

    std::string message_id;
    std::string report_seq;
    std::string body;
    {
        std::lock_guard<std::mutex> lock(alarm_mtx_);
        if(alarm_stream_id_.empty()) {
            return;
        }
        message_id = alarm_next_message_id();
        report_seq = alarm_alloc_report_seq();
        body = build_alarm_status_json(message_id, report_seq);
    }
    alarm_enqueue_and_try_send(std::move(body), false);
}

void MqttService::alarm_request_snapshot_from_glavo()
{
    middleware::Message ntf(middleware::MsgType::UI_Alarm_Snapshot_Ntf,
        middleware::ModuleId::APP_MQTT,
        middleware::ModuleId::APP_GLAVO);
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "UI_Alarm_Snapshot_Ntf publish_async to Glavo failed");
        return;
    }
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Info,
        "requested alarm snapshot from Glavo after MQTT connect");
}

bool MqttService::subscribe_alarm_ack_topic()
{
    const std::string topic = cfg_.topic_alarm_ack();
    const int rc = MQTTClient_subscribe(client_, topic.c_str(), 1);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Error,
            "MQTT subscribe alarm/ack failed rc=%d topic=%s",
            rc, topic.c_str());
        return false;
    }
    alarm_ack_subscribed_ = true;
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Info,
        "MQTT subscribed topic=%s", topic.c_str());
    return true;
}

void MqttService::handle_alarm_ack(const std::string& payload)
{
    std::string type;
    if(!json_extract_string(payload, "type", type) || type != "alarm.receipt") {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "alarm/ack ignored: bad type");
        return;
    }
    std::string device_id;
    if(json_extract_string(payload, "device_id", device_id) &&
       !device_id.empty() && device_id != cfg_.device_id) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "alarm/ack device_id mismatch got=%s", device_id.c_str());
        return;
    }

    /* data 是嵌套对象；用整包扫描 message_id / result（与现有 json_extract 一致）。 */
    std::string message_id;
    std::string result;
    std::string code;
    if(!json_extract_string(payload, "message_id", message_id) ||
       message_id.empty()) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "alarm/ack missing message_id");
        return;
    }
    (void)json_extract_string(payload, "result", result);
    (void)json_extract_string(payload, "code", code);

    const bool accepted =
        (result == "accepted") ||
        (code == "ACCEPTED") ||
        (code == "DUPLICATE");
    const bool rejected =
        (result == "rejected") ||
        (code == "INVALID_PAYLOAD") ||
        (code == "IDENTITY_CONFLICT");

    if(accepted) {
        std::lock_guard<std::mutex> lock(alarm_mtx_);
        for(auto it = alarm_outbox_.begin(); it != alarm_outbox_.end(); ++it) {
            if(it->message_id == message_id) {
                alarm_outbox_erase(message_id);
                alarm_outbox_.erase(it);
                UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Info,
                    "alarm receipt accepted message_id=%s code=%s",
                    message_id.c_str(), code.c_str());
                return;
            }
        }
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Debug,
            "alarm receipt for unknown/already-cleared message_id=%s",
            message_id.c_str());
        return;
    }

    if(rejected) {
        /* 永久拒绝：移出 outbox，避免死循环重发同一坏包。 */
        std::lock_guard<std::mutex> lock(alarm_mtx_);
        for(auto it = alarm_outbox_.begin(); it != alarm_outbox_.end(); ++it) {
            if(it->message_id == message_id) {
                alarm_outbox_erase(message_id);
                alarm_outbox_.erase(it);
                break;
            }
        }
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Error,
            "alarm receipt rejected message_id=%s code=%s",
            message_id.c_str(), code.c_str());
        return;
    }

    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
        "alarm/ack unrecognized result=%s code=%s message_id=%s",
        result.c_str(), code.c_str(), message_id.c_str());
}

void MqttService::on_alarm_report(const middleware::Message& msg)
{
    middleware::AlarmReportPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "invalid GLAVO_Alarm_Report_Ntf payload");
        return;
    }

    std::string message_id;
    std::string report_seq;
    std::string body;
    {
        std::lock_guard<std::mutex> lock(alarm_mtx_);
        if(alarm_stream_id_.empty()) {
            alarm_stream_load();
        }
        message_id = alarm_next_message_id();
        report_seq = alarm_alloc_report_seq();
        body = build_alarm_single_json(p, message_id, report_seq);
    }
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Info,
        "IPC RX alarm.single code=%u state=0x%02X detail=%u -> %s",
        static_cast<unsigned>(p.alarm_code),
        static_cast<unsigned>(p.state_mask),
        static_cast<unsigned>(p.detail),
        message_id.c_str());
    alarm_enqueue_and_try_send(std::move(body), false);
}

void MqttService::on_alarm_cloud_snapshot(const middleware::Message& msg)
{
    middleware::AlarmCloudSnapshotPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Warn,
            "invalid GLAVO_Alarm_Cloud_Snapshot_Ntf payload");
        return;
    }

    std::string message_id;
    std::string report_seq;
    std::string body;
    {
        std::lock_guard<std::mutex> lock(alarm_mtx_);
        if(alarm_stream_id_.empty()) {
            alarm_stream_load();
        }
        alarm_mcu_link_ = p.mcu_link;
        message_id = alarm_next_message_id();
        report_seq = alarm_alloc_report_seq();
        body = build_alarm_snapshot_json(p, message_id, report_seq);
    }
    UPC_LOG_MQTT_SERVICE_TO(MqttTelemetry, Info,
        "IPC RX alarm.snapshot highest=%u mcu_link=%u -> %s",
        static_cast<unsigned>(p.highest_severity),
        static_cast<unsigned>(p.mcu_link),
        message_id.c_str());
    alarm_enqueue_and_try_send(std::move(body), true);
    alarm_maybe_publish_status(true);
}

} // namespace upc::mqtt
