#include "mqtt_service.hpp"

#include "mqtt_service_internal.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace upc::mqtt {

void MqttService::on_ota_user_decision(const middleware::Message& msg)
{
    middleware::OtaDecisionReqPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid UI_Ota_User_Decision_Req payload");
        return;
    }
    p.task_id[sizeof(p.task_id) - 1] = '\0';
    if(p.task_id[0] == '\0') {
        UPC_LOG_MQTT_SERVICE(Warn, "ignore OTA decision with empty task_id");
        return;
    }
    if(p.approve == 0) {
        bool required = false;
        {
            std::lock_guard<std::mutex> lock(ota_mtx_);
            required = pending_ota_notify_.required &&
                       pending_ota_notify_.task_id == p.task_id;
        }
        if(required) {
            UPC_LOG_MQTT_SERVICE(Warn, "ignore cancel for required OTA task_id=%s", p.task_id);
            return;
        }
        UPC_LOG_MQTT_SERVICE(Info, "OTA notify cancelled by UI task_id=%s", p.task_id);
        return;
    }
    (void)publish_manifest_request(p.task_id);
}

void MqttService::handle_ota_notify(const std::string& payload)
{
    OtaNotify notify{};
    if(!parse_ota_notify(payload, notify)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid ota notify payload");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(ota_mtx_);
        pending_ota_notify_ = notify;
    }
    middleware::OtaNotifyPayload ntf{};
    std::snprintf(ntf.task_id, sizeof(ntf.task_id), "%s", notify.task_id.c_str());
    std::snprintf(ntf.release_version, sizeof(ntf.release_version), "%s",
                  notify.release_version.c_str());
    ntf.required = notify.required ? uint8_t(1) : uint8_t(0);
    ntf.approval_status = static_cast<uint8_t>(notify.approval_status);
    middleware::Message msg(middleware::MsgType::MQTT_Ota_Notify_Ntf,
                            middleware::ModuleId::APP_MQTT,
                            middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&ntf), sizeof(ntf));
    (void)middleware::MsgPort::instance().publish_async(msg);
}

void MqttService::handle_ota_manifest_response(const std::string& payload)
{
    std::string task_id;
    std::string err_code;
    std::vector<OtaComponent> components;
    if(!parse_manifest_response(payload, task_id, components, err_code)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid ota manifest response");
        return;
    }
    if(task_id.empty()) {
        UPC_LOG_MQTT_SERVICE(Warn, "ota manifest response without task_id");
        return;
    }
    if(!err_code.empty()) {
        middleware::OtaProgressPayload p{};
        std::snprintf(p.task_id, sizeof(p.task_id), "%s", task_id.c_str());
        p.state = static_cast<uint8_t>(middleware::OtaProgressState::Failed);
        p.progress_pct = 0;
        std::snprintf(p.error_code, sizeof(p.error_code), "%s", err_code.c_str());
        publish_ota_progress_ui(p);
        (void)publish_ota_progress_cloud(p);
        return;
    }
    start_manifest_download(std::move(task_id), std::move(components));
}

bool MqttService::publish_manifest_request(const std::string& task_id)
{
    middleware::McuVersionPayload mcu{};
    middleware::GalvoVersionPayload galvo{};
    bool mcu_valid = false;
    bool galvo_valid = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        mcu = fw_cache_.mcu;
        galvo = fw_cache_.galvo;
        mcu_valid = fw_cache_.mcu_valid;
        galvo_valid = fw_cache_.galvo_valid;
    }
    const std::string mcu_version = mcu_valid ? format_mcu_version(mcu) : "unknown";
    const std::string galvo_version = galvo_valid ? format_galvo_version(galvo.fw_version_le)
                                                  : "unknown";
    const std::string req_id = std::string("manifest-") + task_id;
    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"ota_manifest_request\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"req_id\":\"%s\","
        "\"data\":{"
            "\"task_id\":\"%s\","
            "\"versions\":{"
                "\"soc\":\"%s\","
                "\"mcu\":\"%s\","
                "\"glavo\":\"%s\""
            "}"
        "}"
        "}",
        json_escape(cfg_.device_id).c_str(),
        ts,
        seq,
        json_escape(req_id).c_str(),
        json_escape(task_id).c_str(),
        json_escape(UPC_SW_VERSION).c_str(),
        json_escape(mcu_version).c_str(),
        json_escape(galvo_version).c_str());
    const std::string topic = cfg_.topic_ota_manifest_req();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
    msg.qos = cfg_.qos_ack;
    msg.retained = 0;

    debug_log_mqtt_frame("TX", topic, msg.payload,
                         static_cast<size_t>(msg.payloadlen),
                         msg.qos, msg.retained);

    MQTTClient_deliveryToken token{};
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish ota manifest request failed rc=%d", rc);
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        ota_subscribed_ = false;
        return false;
    }
    UPC_LOG_MQTT_SERVICE(Info, "ota manifest request sent task_id=%s token=%d",
                         task_id.c_str(), static_cast<int>(token));
    return true;
}

bool MqttService::publish_ota_progress_cloud(const middleware::OtaProgressPayload& progress)
{
    const char* state = "downloading";
    switch(static_cast<middleware::OtaProgressState>(progress.state)) {
    case middleware::OtaProgressState::Verifying: state = "verifying"; break;
    case middleware::OtaProgressState::Applying: state = "applying"; break;
    case middleware::OtaProgressState::Completed: state = "completed"; break;
    case middleware::OtaProgressState::Failed: state = "failed"; break;
    case middleware::OtaProgressState::RolledBack: state = "rolled_back"; break;
    case middleware::OtaProgressState::Downloading:
    default: break;
    }
    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"ota_progress\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"data\":{"
            "\"task_id\":\"%s\","
            "\"component\":\"%s\","
            "\"state\":\"%s\","
            "\"progress_pct\":%u,"
            "\"error_code\":\"%s\""
        "}"
        "}",
        json_escape(cfg_.device_id).c_str(),
        static_cast<long long>(unix_ms()),
        ++seq_,
        json_escape(progress.task_id).c_str(),
        json_escape(progress.component).c_str(),
        state,
        static_cast<unsigned>(progress.progress_pct),
        json_escape(progress.error_code).c_str());
    const std::string topic = cfg_.topic_ota_progress();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
    msg.qos = cfg_.qos_ack;
    msg.retained = 0;
    MQTTClient_deliveryToken token{};
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish ota progress failed rc=%d", rc);
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        ota_subscribed_ = false;
        return false;
    }
    return true;
}

void MqttService::publish_ota_progress_ui(const middleware::OtaProgressPayload& progress)
{
    middleware::Message msg(middleware::MsgType::APP_Mqtt_Ota_Progress_Ntf,
                            middleware::ModuleId::APP_MQTT,
                            middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&progress), sizeof(progress));
    (void)middleware::MsgPort::instance().publish_async(msg);
}

void MqttService::start_manifest_download(std::string task_id,
                                          std::vector<OtaComponent> components)
{
    {
        std::lock_guard<std::mutex> lock(ota_mtx_);
        if(ota_download_running_) {
            UPC_LOG_MQTT_SERVICE(Warn, "ota download already running, ignore task_id=%s",
                                 task_id.c_str());
            return;
        }
        ota_download_running_ = true;
    }
    if(ota_download_thread_.joinable()) {
        ota_download_thread_.join();
    }
    ota_download_thread_ = std::thread(
        [this, task_id = std::move(task_id), components = std::move(components)]() mutable {
            run_manifest_download_task(std::move(task_id), std::move(components));
            std::lock_guard<std::mutex> lock(ota_mtx_);
            ota_download_running_ = false;
        });
}

void MqttService::run_manifest_download_task(std::string task_id,
                                             std::vector<OtaComponent> components)
{
    auto publish_failed = [this, &task_id](const char* component, const char* code) {
        middleware::OtaProgressPayload p{};
        std::snprintf(p.task_id, sizeof(p.task_id), "%s", task_id.c_str());
        std::snprintf(p.component, sizeof(p.component), "%s", component ? component : "");
        p.state = static_cast<uint8_t>(middleware::OtaProgressState::Failed);
        p.progress_pct = 0;
        std::snprintf(p.error_code, sizeof(p.error_code), "%s", code ? code : "unknown");
        publish_ota_progress_ui(p);
        (void)publish_ota_progress_cloud(p);
    };

    if(components.empty()) {
        publish_failed("all", "empty_manifest");
        return;
    }

    if(!ensure_dir(kImageTmpDir)) {
        publish_failed("all", "tmp_dir_failed");
        return;
    }

    bool handled_any_component = false;
    for(const OtaComponent& comp : components) {
        const char* target_dir = component_target_dir(comp.component);
        if(!target_dir) {
            publish_failed(comp.component.c_str(), "invalid_component");
            return;
        }
        handled_any_component = true;

        middleware::OtaProgressPayload progress{};
        std::snprintf(progress.task_id, sizeof(progress.task_id), "%s", task_id.c_str());
        std::snprintf(progress.component, sizeof(progress.component), "%s", comp.component.c_str());
        progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Downloading);
        progress.progress_pct = 0;
        publish_ota_progress_ui(progress);
        (void)publish_ota_progress_cloud(progress);

        const std::string base_name = comp.file_name.empty()
            ? basename_from_url_path(comp.url)
            : comp.file_name;
        const std::string upper_base = to_upper_ascii(base_name);
        const bool is_tar_gz =
            (upper_base.size() >= 7 &&
             upper_base.rfind(".TAR.GZ") == upper_base.size() - 7) ||
            (upper_base.size() >= 4 &&
             upper_base.rfind(".TGZ") == upper_base.size() - 4);
        if(!is_tar_gz) {
            publish_failed(comp.component.c_str(), "unsupported_package");
            return;
        }

        const std::filesystem::path tmp_part =
            std::filesystem::path(kImageTmpDir) /
            (task_id + "_" + comp.component + "_" + base_name + ".part");
        const std::filesystem::path tmp_final =
            std::filesystem::path(kImageTmpDir) /
            (task_id + "_" + comp.component + "_" + base_name);
        const std::filesystem::path extract_dir =
            std::filesystem::path(kImageTmpDir) /
            (task_id + "_" + comp.component + "_extract");

        std::error_code ec;
        std::filesystem::remove(tmp_part, ec);
        std::filesystem::remove(tmp_final, ec);
        std::filesystem::remove_all(extract_dir, ec);

        std::string dl_err;
        const bool dl_ok = download_file_with_progress(
            comp.url, tmp_part,
            [this, &progress](size_t recv, size_t total) {
                if(total > 0) {
                    const int pct = static_cast<int>((recv * 100) / total);
                    progress.progress_pct = static_cast<uint8_t>(std::clamp(pct, 0, 80));
                } else if(progress.progress_pct < 80) {
                    progress.progress_pct = static_cast<uint8_t>(
                        std::min<int>(80, static_cast<int>(progress.progress_pct) + 1));
                }
                publish_ota_progress_ui(progress);
                (void)publish_ota_progress_cloud(progress);
            }, dl_err);
        if(!dl_ok) {
            std::filesystem::remove(tmp_part, ec);
            publish_failed(comp.component.c_str(), dl_err.c_str());
            return;
        }

        std::filesystem::rename(tmp_part, tmp_final, ec);
        if(ec) {
            std::filesystem::remove(tmp_part, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "tmp_rename_failed");
            return;
        }

        progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Verifying);
        progress.progress_pct = 90;
        publish_ota_progress_ui(progress);
        (void)publish_ota_progress_cloud(progress);

        const std::string local_sha = hex_sha256_file(tmp_final);
        std::string expected_sha = comp.sha256;
        std::transform(expected_sha.begin(), expected_sha.end(), expected_sha.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if(!expected_sha.empty() && local_sha != expected_sha) {
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "sha256_mismatch");
            return;
        }

        if(!ensure_dir(extract_dir)) {
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "extract_dir_failed");
            return;
        }

        std::vector<std::string> tar_entries;
        const std::string list_cmd =
            "tar -tzf " + sh_single_quote(tmp_final.string());
        if(!run_capture_lines(list_cmd, tar_entries)) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "tar_list_failed");
            return;
        }
        bool has_app1 = false;
        bool has_app2 = false;
        for(const std::string& entry_raw : tar_entries) {
            const std::string entry = trim_line(entry_raw);
            if(entry.empty()) {
                continue;
            }
            if(!archive_entry_safe_flat(entry) || !is_bin_name(entry)) {
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "tar_entry_invalid");
                return;
            }
            const std::string upper = to_upper_ascii(entry);
            if(upper.find("APP1") != std::string::npos) {
                has_app1 = true;
            }
            if(upper.find("APP2") != std::string::npos) {
                has_app2 = true;
            }
        }
        if(!has_app1 || !has_app2) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "tar_missing_ab");
            return;
        }

        const std::string extract_cmd =
            "tar -xzf " + sh_single_quote(tmp_final.string()) +
            " -C " + sh_single_quote(extract_dir.string());
        if(!run_shell_ok(extract_cmd)) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "tar_extract_failed");
            return;
        }

        bool extracted_app1 = false;
        bool extracted_app2 = false;
        std::vector<std::filesystem::path> extracted_files;
        for(const auto& ent : std::filesystem::directory_iterator(extract_dir, ec)) {
            if(ec || !ent.is_regular_file()) {
                continue;
            }
            const std::string name = ent.path().filename().string();
            if(!is_bin_name(name)) {
                continue;
            }
            extracted_files.push_back(ent.path());
            const std::string upper = to_upper_ascii(name);
            if(upper.find("APP1") != std::string::npos) {
                extracted_app1 = true;
            }
            if(upper.find("APP2") != std::string::npos) {
                extracted_app2 = true;
            }
        }
        if(!extracted_app1 || !extracted_app2 || extracted_files.size() != 2) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "extract_ab_invalid");
            return;
        }

        progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Applying);
        progress.progress_pct = 100;
        publish_ota_progress_ui(progress);
        (void)publish_ota_progress_cloud(progress);

        if(!ensure_dir(target_dir)) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "target_dir_failed");
            return;
        }

        const std::filesystem::path backup_dir =
            std::filesystem::path(kImageTmpDir) /
            (task_id + "_" + comp.component + "_backup");
        std::filesystem::remove_all(backup_dir, ec);
        if(!ensure_dir(backup_dir)) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "backup_dir_failed");
            return;
        }

        std::vector<std::filesystem::path> backup_paths;
        for(const auto& ent : std::filesystem::directory_iterator(target_dir, ec)) {
            if(ec || !ent.is_regular_file()) {
                continue;
            }
            const std::filesystem::path dst = backup_dir / ent.path().filename();
            std::filesystem::rename(ent.path(), dst, ec);
            if(ec) {
                for(const auto& back : backup_paths) {
                    const std::filesystem::path restore = std::filesystem::path(target_dir) / back.filename();
                    std::filesystem::rename(back, restore, ec);
                }
                std::filesystem::remove_all(backup_dir, ec);
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "backup_old_failed");
                return;
            }
            backup_paths.push_back(dst);
        }

        std::vector<std::filesystem::path> moved_new_files;
        for(const auto& src : extracted_files) {
            const auto dst = std::filesystem::path(target_dir) / src.filename();
            std::filesystem::rename(src, dst, ec);
            if(ec) {
                for(const auto& moved : moved_new_files) {
                    std::filesystem::remove(moved, ec);
                }
                for(const auto& back : backup_paths) {
                    const std::filesystem::path restore = std::filesystem::path(target_dir) / back.filename();
                    std::filesystem::rename(back, restore, ec);
                }
                std::filesystem::remove_all(backup_dir, ec);
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "move_failed");
                return;
            }
            moved_new_files.push_back(dst);
        }

        std::filesystem::remove_all(backup_dir, ec);
        std::filesystem::remove_all(extract_dir, ec);
        std::filesystem::remove(tmp_final, ec);

        progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Completed);
        progress.progress_pct = 100;
        progress.error_code[0] = '\0';
        publish_ota_progress_ui(progress);
        (void)publish_ota_progress_cloud(progress);

        if(!comp.version.empty()) {
            std::lock_guard<std::mutex> lock(data_mtx_);
            if(comp.component == "soc" && env_sim_on_) {
                soc_fw_override_ = comp.version;
            } else if(comp.component == "mcu" && env_sim_on_) {
                middleware::McuVersionPayload parsed{};
                if(parse_mcu_version_text(comp.version, parsed)) {
                    fw_cache_.mcu = parsed;
                    fw_cache_.mcu_valid = true;
                }
            } else if(comp.component == "glavo" && env_sim_on_) {
                middleware::GalvoVersionPayload parsed{};
                if(parse_galvo_version_text(comp.version, parsed)) {
                    fw_cache_.galvo = parsed;
                    fw_cache_.galvo_valid = true;
                }
            }
        }
    }

    if(!handled_any_component) {
        publish_failed("all", "no_supported_component");
        return;
    }

    bool need_mcu_refresh = false;
    bool need_galvo_refresh = false;
    bool env_sim_on = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        env_sim_on = env_sim_on_;
    }
    for(const OtaComponent& comp : components) {
        if(comp.component == "mcu") {
            need_mcu_refresh = true;
        } else if(comp.component == "glavo") {
            need_galvo_refresh = true;
        }
    }

    if(env_sim_on) {
        for(const OtaComponent& comp : components) {
            if(comp.component == "mcu" && !comp.version.empty()) {
                need_mcu_refresh = false;
            } else if(comp.component == "glavo" && !comp.version.empty()) {
                need_galvo_refresh = false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        ota_hb_wait_.active = (need_mcu_refresh || need_galvo_refresh);
        ota_hb_wait_.need_mcu = need_mcu_refresh;
        ota_hb_wait_.need_galvo = need_galvo_refresh;
        ota_hb_wait_.got_mcu = false;
        ota_hb_wait_.got_galvo = false;
        ota_hb_wait_.deadline_ms = monotonic_ms() + 3000;
    }

    if(need_mcu_refresh || need_galvo_refresh) {
        request_firmware_versions_force(need_mcu_refresh, need_galvo_refresh);
    }

    if(!need_mcu_refresh && !need_galvo_refresh) {
        last_heartbeat_pub_ms_ = 0;
        (void)publish_heartbeat();
    }
}

bool MqttService::parse_ota_notify(const std::string& payload, OtaNotify& out) const
{
    out = OtaNotify{};
    std::string data;
    if(!json_extract_object(payload, "data", data)) {
        return false;
    }
    if(!json_extract_string(data, "task_id", out.task_id)) {
        (void)json_extract_string(data, "taskId", out.task_id);
    }
    if(out.task_id.empty()) {
        return false;
    }
    (void)json_extract_string(data, "release_version", out.release_version);
    if(out.release_version.empty()) {
        (void)json_extract_string(data, "releaseVersion", out.release_version);
    }
    bool required = false;
    if(json_extract_bool(data, "required", required)) {
        out.required = required;
    }
    std::string approval;
    if(json_extract_string(data, "approval_status", approval) ||
       json_extract_string(data, "approvalStatus", approval)) {
        (void)map_approval_status(approval, out.approval_status);
    }
    return true;
}

bool MqttService::parse_manifest_response(const std::string& payload,
                                          std::string& task_id,
                                          std::vector<OtaComponent>& components,
                                          std::string& err_code) const
{
    task_id.clear();
    components.clear();
    err_code.clear();
    std::string data;
    if(!json_extract_object(payload, "data", data)) {
        return false;
    }
    (void)json_extract_string(data, "task_id", task_id);
    if(task_id.empty()) {
        (void)json_extract_string(data, "taskId", task_id);
    }
    (void)json_extract_string(data, "error_code", err_code);
    if(err_code.empty()) {
        (void)json_extract_string(data, "errorCode", err_code);
    }

    std::vector<std::string> objs;
    if(!json_extract_array_objects(data, "components", objs)) {
        return !task_id.empty();
    }
    for(const std::string& obj : objs) {
        OtaComponent c{};
        if(!json_extract_string(obj, "component", c.component) || c.component.empty()) {
            continue;
        }
        (void)json_extract_string(obj, "version", c.version);
        (void)json_extract_string(obj, "url", c.url);
        (void)json_extract_string(obj, "sha256", c.sha256);
        (void)json_extract_string(obj, "file_name", c.file_name);
        if(c.file_name.empty()) {
            (void)json_extract_string(obj, "filename", c.file_name);
        }
        int size_bytes = 0;
        if(json_extract_int(obj, "size_bytes", size_bytes) && size_bytes > 0) {
            c.size_bytes = static_cast<uint64_t>(size_bytes);
        }
        if(c.url.empty()) {
            continue;
        }
        if(c.file_name.empty()) {
            ParsedUrl parsed{};
            if(parse_url(c.url, parsed)) {
                c.file_name = basename_from_url_path(parsed.path);
            }
        }
        components.push_back(std::move(c));
    }
    return !task_id.empty();
}

} // namespace upc::mqtt
