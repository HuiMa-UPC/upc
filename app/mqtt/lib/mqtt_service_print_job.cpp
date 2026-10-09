/**
 * @file mqtt_service_print_job.cpp
 * @brief 云端打印任务上报。切片导入只负责把小缓存放进暂存目录；
 *        本线程在后台要 job_id、报 plate_meta、PUT 原始 PNG。
 *        云端失败或一直没回包都只删这份缓存，不挡传输，也不挡开打。
 */

#include "mqtt_service.hpp"
#include "mqtt_service_internal.hpp"

#include "gcode_manifest.hpp"
#include "xfr_paths.hpp"

#include <MQTTClient.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

namespace upc::mqtt {

namespace {

constexpr int kPrintJobMaxAttempts = 8;
constexpr int64_t kPrintJobGiveUpMs = 30LL * 60LL * 1000LL;
constexpr int kPrintJobAckWaitSec = 15;
constexpr int kPrintJobPutTries = 3;
constexpr int kJobDone = 0;
constexpr int kJobRetry = 1;
constexpr int kJobWaitMqtt = 2;

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if(!in) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool write_file(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if(!out) {
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

std::string trim_copy(std::string text)
{
    while(!text.empty()) {
        const unsigned char c = static_cast<unsigned char>(text.back());
        if(c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        text.pop_back();
    }
    size_t begin = 0;
    while(begin < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[begin]);
        if(c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        ++begin;
    }
    return text.substr(begin);
}

bool is_hex32(const std::string& text)
{
    if(text.size() != 32) {
        return false;
    }
    for(unsigned char c : text) {
        if(std::isxdigit(c) == 0) {
            return false;
        }
    }
    return true;
}

bool is_json_object(const std::string& raw)
{
    const std::string text = trim_copy(raw);
    return text.size() >= 2 && text.front() == '{' && text.back() == '}';
}

size_t utf8_codepoints(const std::string& text)
{
    size_t count = 0;
    for(unsigned char c : text) {
        if((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

int read_int_file(const std::filesystem::path& path)
{
    std::ifstream in(path);
    int value = 0;
    if(!(in >> value) || value < 0) {
        return 0;
    }
    return value;
}

int64_t read_i64_file(const std::filesystem::path& path)
{
    std::ifstream in(path);
    int64_t value = 0;
    if(!(in >> value) || value < 0) {
        return 0;
    }
    return value;
}

} // namespace

void MqttService::enqueue_existing_print_jobs()
{
    std::error_code ec;
    std::vector<std::string> found;
    const std::filesystem::path root(UPC_PRINT_JOB_STAGING_DIR);
    if(std::filesystem::exists(root, ec) && !ec) {
        std::filesystem::directory_iterator it(root, ec);
        const std::filesystem::directory_iterator end;
        for(; !ec && it != end; it.increment(ec)) {
            std::error_code dir_ec;
            if(!it->is_directory(dir_ec) || dir_ec) {
                continue;
            }
            const std::string name = it->path().filename().string();
            if(!is_hex32(name)) {
                continue;
            }
            found.push_back(it->path().string());
        }
    }

    std::lock_guard<std::mutex> lock(print_job_mtx_);
    for(std::string& path : found) {
        if(std::find(print_job_queue_.begin(), print_job_queue_.end(), path) ==
           print_job_queue_.end()) {
            print_job_queue_.push_back(std::move(path));
        }
    }
}

void MqttService::start_print_job_worker()
{
    if(print_job_thread_.joinable()) {
        return;
    }
    print_job_stop_.store(false);
    enqueue_existing_print_jobs();
    print_job_thread_ = std::thread([this] { print_job_worker_loop(); });
}

void MqttService::stop_print_job_worker()
{
    print_job_stop_.store(true);
    print_job_cv_.notify_all();
    print_job_ack_cv_.notify_all();
    if(print_job_thread_.joinable()) {
        print_job_thread_.join();
    }
}

void MqttService::print_job_worker_loop()
{
    while(!print_job_stop_.load()) {
        std::string dir;
        {
            std::unique_lock<std::mutex> lock(print_job_mtx_);
            if(print_job_queue_.empty()) {
                print_job_cv_.wait_for(lock, std::chrono::seconds(20), [this] {
                    return print_job_stop_.load() || !print_job_queue_.empty();
                });
                if(print_job_stop_.load()) {
                    return;
                }
                if(print_job_queue_.empty()) {
                    lock.unlock();
                    /* 通知丢了也还能捞到暂存目录，不必等重启。 */
                    enqueue_existing_print_jobs();
                    continue;
                }
            }
            dir = std::move(print_job_queue_.front());
            print_job_queue_.erase(print_job_queue_.begin());
        }

        const int step = process_print_job_staging(dir);
        if(print_job_stop_.load() || step == kJobDone) {
            continue;
        }

        const auto delay = (step == kJobWaitMqtt)
            ? std::chrono::seconds(5)
            : std::chrono::seconds(2);
        std::unique_lock<std::mutex> lock(print_job_mtx_);
        print_job_cv_.wait_for(lock, delay, [this] {
            return print_job_stop_.load();
        });
        if(!print_job_stop_.load()) {
            print_job_queue_.push_back(std::move(dir));
        }
    }
}

void MqttService::on_print_job_report(const middleware::Message& msg)
{
    middleware::PrintJobReportNtfPayload payload{};
    if(!read_inline_pod(msg.payload, payload)) {
        return;
    }
    payload.staging_dir[sizeof(payload.staging_dir) - 1] = '\0';
    if(payload.staging_dir[0] == '\0') {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(print_job_mtx_);
        print_job_queue_.push_back(payload.staging_dir);
    }
    print_job_cv_.notify_one();
}

void MqttService::subscribe_print_job_ack()
{
    if(!client_) {
        return;
    }
    const std::string topic = cfg_.topic_print_job_ack();
    const int rc = MQTTClient_subscribe(client_, topic.c_str(), 1);
    if(rc != MQTTCLIENT_SUCCESS) {
        /* 订阅失败不能拆掉整条 MQTT。云端规则没开时，上报放弃即可。 */
        UPC_LOG_MQTT_SERVICE(Warn, "MQTT subscribe print_job/ack failed rc=%d topic=%s",
                             rc, topic.c_str());
        return;
    }
    UPC_LOG_MQTT_SERVICE(Info, "MQTT subscribed topic=%s", topic.c_str());
}

void MqttService::handle_print_job_ack(const std::string& payload)
{
    std::string req_id;
    std::string data;
    std::string job_id;
    std::string upload_url;
    std::string object_key;
    int64_t expires_at = 0;
    (void)json_extract_string(payload, "req_id", req_id);
    if(json_extract_object(payload, "data", data)) {
        (void)json_extract_string(data, "job_id", job_id);
        (void)json_extract_string(data, "uploadUrl", upload_url);
        (void)json_extract_string(data, "objectKey", object_key);
        (void)json_extract_int64(data, "uploadUrlExpiresAt", expires_at);
    }

    std::lock_guard<std::mutex> lock(print_job_ack_mtx_);
    if(req_id.empty() || req_id != print_job_wait_req_ || !is_hex32(job_id)) {
        UPC_LOG_MQTT_SERVICE(Debug, "print job ack ignored req=%s job=%s",
                             req_id.c_str(), job_id.c_str());
        return;
    }
    print_job_ack_job_id_ = job_id;
    print_job_ack_upload_url_ = upload_url;
    print_job_ack_object_key_ = object_key;
    print_job_ack_expires_ = expires_at;
    print_job_ack_ready_ = true;
    print_job_ack_cv_.notify_all();
}

bool MqttService::publish_mqtt_qos1(const std::string& topic, const std::string& payload)
{
    if(!mqtt_connected_ || !client_) {
        return false;
    }
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = const_cast<char*>(payload.data());
    msg.payloadlen = static_cast<int>(payload.size());
    msg.qos = 1;
    msg.retained = 0;
    MQTTClient_deliveryToken token = 0;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "print job publish failed rc=%d topic=%s",
                             rc, topic.c_str());
        mqtt_connected_ = false;
        return false;
    }
    return true;
}

std::string MqttService::build_uplink_json(const char* type,
                                           const std::string& req_id,
                                           const std::string& data_json)
{
    const uint32_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    std::ostringstream oss;
    oss << "{\"v\":1"
        << ",\"type\":\"" << type << "\""
        << ",\"device_id\":\"" << json_escape(cfg_.device_id) << "\""
        << ",\"ts\":" << unix_ms()
        << ",\"seq\":" << seq;
    if(!req_id.empty()) {
        oss << ",\"req_id\":\"" << json_escape(req_id) << "\"";
    }
    oss << ",\"data\":" << data_json << "}";
    return oss.str();
}

bool MqttService::request_print_job_ack(const std::string& req_id,
                                        std::string& job_id,
                                        std::string& upload_url,
                                        std::string& object_key,
                                        int64_t& expires_at)
{
    {
        std::lock_guard<std::mutex> lock(print_job_ack_mtx_);
        print_job_wait_req_ = req_id;
        print_job_ack_ready_ = false;
        print_job_ack_job_id_.clear();
        print_job_ack_upload_url_.clear();
        print_job_ack_object_key_.clear();
        print_job_ack_expires_ = 0;
    }

    const std::string body = build_uplink_json("print_job/request", req_id, "{}");
    if(!publish_mqtt_qos1(cfg_.topic_print_job_request(), body)) {
        return false;
    }

    std::unique_lock<std::mutex> lock(print_job_ack_mtx_);
    print_job_ack_cv_.wait_for(lock, std::chrono::seconds(kPrintJobAckWaitSec), [this] {
        return print_job_stop_.load() || print_job_ack_ready_;
    });
    if(print_job_stop_.load() || !print_job_ack_ready_) {
        return false;
    }
    job_id = print_job_ack_job_id_;
    upload_url = print_job_ack_upload_url_;
    object_key = print_job_ack_object_key_;
    expires_at = print_job_ack_expires_;
    print_job_ack_ready_ = false;
    return is_hex32(job_id);
}

bool MqttService::bind_cloud_job_id(const std::string& gcode_name,
                                    const std::string& req_id,
                                    const std::string& job_id)
{
    if(gcode_name.empty() || !is_hex32(req_id) || !is_hex32(job_id)) {
        return true;
    }

    std::vector<GcodeManifestEntry> entries;
    if(!gcode_manifest_load(UPC_LOCAL_GCODE_CONFIG, entries)) {
        return false;
    }

    bool matched = false;
    bool dirty = false;
    for(GcodeManifestEntry& entry : entries) {
        if(entry.name != gcode_name || entry.cloud_req_id != req_id) {
            continue;
        }
        matched = true;
        if(entry.cloud_job_id != job_id) {
            entry.cloud_job_id = job_id;
            dirty = true;
        }
        break;
    }
    if(!matched) {
        /* 同名文件又导入了一次，旧 req_id 不能把 job_id 写到新记录上。 */
        UPC_LOG_MQTT_SERVICE(Info, "print job bind skipped req=%s gcode=%s",
                             req_id.c_str(), gcode_name.c_str());
        return true;
    }
    notify_glavo_print_job(req_id, job_id);
    if(dirty && !gcode_manifest_save(UPC_LOCAL_GCODE_CONFIG, entries)) {
        return false;
    }
    return true;
}

void MqttService::notify_glavo_print_job(const std::string& req_id, const std::string& job_id)
{
    middleware::PrintJobIdNtfPayload payload{};
    std::snprintf(payload.req_id, sizeof(payload.req_id), "%s", req_id.c_str());
    std::snprintf(payload.job_id, sizeof(payload.job_id), "%s", job_id.c_str());
    middleware::Message msg(middleware::MsgType::MQTT_Print_Job_Id_Ntf,
                            middleware::ModuleId::APP_MQTT,
                            middleware::ModuleId::APP_GLAVO);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    (void)middleware::MsgPort::instance().publish_async(msg);
}

int MqttService::process_print_job_staging(const std::string& staging_dir)
{
    std::error_code ec;
    const std::filesystem::path dir(staging_dir);
    if(!std::filesystem::is_directory(dir, ec) || ec) {
        return kJobDone;
    }
    const std::string req_id = dir.filename().string();
    if(!is_hex32(req_id)) {
        UPC_LOG_MQTT_SERVICE(Warn, "print job staging ignored path=%s", staging_dir.c_str());
        return kJobDone;
    }

    const std::filesystem::path started_path = dir / "started_ms";
    int64_t started_ms = read_i64_file(started_path);
    if(started_ms <= 0) {
        started_ms = unix_ms();
        (void)write_file(started_path, std::to_string(started_ms));
    }
    int attempts = read_int_file(dir / "attempts");
    const int64_t age_ms = unix_ms() - started_ms;
    if(attempts >= kPrintJobMaxAttempts || age_ms >= kPrintJobGiveUpMs) {
        UPC_LOG_MQTT_SERVICE(Warn,
            "print job give up req=%s attempts=%d age_ms=%lld",
            req_id.c_str(), attempts, static_cast<long long>(age_ms));
        std::filesystem::remove_all(dir, ec);
        return kJobDone;
    }
    if(!mqtt_connected_) {
        return kJobWaitMqtt;
    }

    ++attempts;
    (void)write_file(dir / "attempts", std::to_string(attempts));

    std::string job_id;
    std::string upload_url;
    std::string object_key;
    int64_t expires_at = 0;
    if(!request_print_job_ack(req_id, job_id, upload_url, object_key, expires_at)) {
        UPC_LOG_MQTT_SERVICE(Warn, "print job ack timeout req=%s attempt=%d",
                             req_id.c_str(), attempts);
        return kJobRetry;
    }

    const std::string gcode_name = trim_copy(read_file(dir / "gcode_name"));
    const std::string model_name = trim_copy(read_file(dir / "model_name"));
    const std::string plate_meta = read_file(dir / "plate_meta.json");
    if(!bind_cloud_job_id(gcode_name, req_id, job_id)) {
        return kJobRetry;
    }

    const size_t model_cps = utf8_codepoints(model_name);
    if(model_name.empty() || model_cps > 256 || !is_json_object(plate_meta)) {
        UPC_LOG_MQTT_SERVICE(Info,
            "print job report skipped req=%s model_cps=%zu meta_object=%d",
            req_id.c_str(), model_cps, is_json_object(plate_meta) ? 1 : 0);
    } else {
        const std::string data =
            std::string("{\"job_id\":\"") + json_escape(job_id) +
            "\",\"model_name\":\"" + json_escape(model_name) +
            "\",\"plate_meta\":" + trim_copy(plate_meta) + "}";
        const std::string body = build_uplink_json("print_task/report", "", data);
        if(!publish_mqtt_qos1(cfg_.topic_print_task_report(), body)) {
            return kJobRetry;
        }
    }

    const std::filesystem::path png_path = dir / "thumb.png";
    if(!std::filesystem::is_regular_file(png_path, ec) || ec) {
        std::filesystem::remove_all(dir, ec);
        UPC_LOG_MQTT_SERVICE(Info, "print job reported without png req=%s job=%s",
                             req_id.c_str(), job_id.c_str());
        return kJobDone;
    }

    const int64_t now_sec = unix_ms() / 1000;
    if(upload_url.empty() || (expires_at > 0 && now_sec + 30 >= expires_at)) {
        if(!request_print_job_ack(req_id, job_id, upload_url, object_key, expires_at) ||
           upload_url.empty()) {
            return kJobRetry;
        }
        if(!bind_cloud_job_id(gcode_name, req_id, job_id)) {
            return kJobRetry;
        }
    }

    const std::string png = read_file(png_path);
    bool put_ok = false;
    std::string put_err;
    for(int i = 0; i < kPrintJobPutTries && !print_job_stop_.load(); ++i) {
        put_err.clear();
        if(upload_http_put(upload_url, png.data(), png.size(), put_err, {})) {
            put_ok = true;
            break;
        }
        UPC_LOG_MQTT_SERVICE(Warn, "print job put failed req=%s try=%d err=%s",
                             req_id.c_str(), i + 1, put_err.c_str());
    }
    if(!put_ok) {
        return kJobRetry;
    }

    const std::string thumb_data =
        std::string("{\"job_id\":\"") + json_escape(job_id) +
        "\",\"objectKey\":\"" + json_escape(object_key) + "\"}";
    const std::string thumb_body =
        build_uplink_json("print_thumbnail/ack", "", thumb_data);
    if(!publish_mqtt_qos1(cfg_.topic_print_thumbnail_ack(), thumb_body)) {
        return kJobRetry;
    }

    std::filesystem::remove_all(dir, ec);
    UPC_LOG_MQTT_SERVICE(Info, "print job reported req=%s job=%s",
                         req_id.c_str(), job_id.c_str());
    return kJobDone;
}

} // namespace upc::mqtt
