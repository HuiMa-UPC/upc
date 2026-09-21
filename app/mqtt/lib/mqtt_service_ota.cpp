#include "mqtt_service.hpp"

#include "mqtt_service_internal.hpp"
#include "ota_image_pair.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace upc::mqtt {

/**
 * OTA 入口：处理 UI 的确认/取消指令。
 * - 取消时会校验是否为 required 任务，required 任务不允许取消。
 * - 确认后进入 manifest 请求阶段。
 */
void MqttService::on_ota_user_decision(const middleware::Message& msg)
{
    middleware::OtaDecisionReqPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "invalid UI_Ota_User_Decision_Req payload");
        return;
    }
    p.task_id[sizeof(p.task_id) - 1] = '\0';
    if(p.task_id[0] == '\0') {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "ignore OTA decision with empty task_id");
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
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "ignore cancel for required OTA task_id=%s", p.task_id);
            return;
        }
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info, "OTA notify cancelled by UI task_id=%s", p.task_id);
        return;
    }
    (void)publish_manifest_request(p.task_id);
}

void MqttService::handle_ota_notify(const std::string& payload)
{
    OtaNotify notify{};
    if(!parse_ota_notify(payload, notify)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "invalid ota notify payload");
        return;
    }
    {
        // 缓存最近一次 OTA 通知，供后续 UI 决策校验（如 required 任务不可取消）。
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
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "invalid ota manifest response");
        return;
    }
    if(task_id.empty()) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "ota manifest response without task_id");
        return;
    }
    // 云端显式返回错误码，直接上报失败并终止当前 OTA 流。
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
        // 使用当前缓存版本填充 manifest 请求，帮助云端做差分或目标包决策。
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
                "\"galvo\":\"%s\""
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
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "publish ota manifest request failed rc=%d", rc);
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        ota_subscribed_ = false;
        mgmt_subscribed_ = false;
        return false;
    }
    UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info, "ota manifest request sent task_id=%s token=%d",
                         task_id.c_str(), static_cast<int>(token));
    return true;
}

bool MqttService::publish_ota_progress_cloud(const middleware::OtaProgressPayload& progress)
{
    // IPC 枚举状态映射到云端协议字符串。
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
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "publish ota progress failed rc=%d", rc);
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        ota_subscribed_ = false;
        mgmt_subscribed_ = false;
        return false;
    }
    return true;
}

void MqttService::publish_ota_progress_ui(const middleware::OtaProgressPayload& progress)
{
    // 统一走异步消息发给 UI，避免阻塞 MQTT 主循环。
    middleware::Message msg(middleware::MsgType::APP_Mqtt_Ota_Progress_Ntf,
                            middleware::ModuleId::APP_MQTT,
                            middleware::ModuleId::UI);
    const auto state = static_cast<middleware::OtaProgressState>(progress.state);
    if(state == middleware::OtaProgressState::Completed ||
       state == middleware::OtaProgressState::Failed ||
       state == middleware::OtaProgressState::RolledBack) {
        msg.priority = middleware::MsgPriority::High;
    } else {
        msg.priority = middleware::MsgPriority::Normal;
    }
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&progress), sizeof(progress));
    (void)middleware::MsgPort::instance().publish_async(msg);
}

void MqttService::on_apply_ota_status(const middleware::Message& msg,
                                      ApplyOtaSync& sync,
                                      std::mutex& mtx,
                                      std::condition_variable& cv,
                                      const char* invalid_log,
                                      const char* fail_code)
{
    middleware::McuOtaStatusPayload st{};
    if(!read_inline_pod(msg.payload, st)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "%s", invalid_log ? invalid_log : "invalid OTA status payload");
        return;
    }

    const auto state = static_cast<middleware::McuOtaState>(st.state);
    const bool terminal =
        (state == middleware::McuOtaState::Success) ||
        (state == middleware::McuOtaState::Failed) ||
        (state == middleware::McuOtaState::Cancelled);
    auto map_apply_progress = [](uint8_t raw) -> uint8_t {
        const int mapped = 90 + (static_cast<int>(raw) * 9) / 255;
        return static_cast<uint8_t>(std::clamp(mapped, 90, 99));
    };

    std::string task_id;
    std::string component;
    middleware::OtaProgressPayload p{};
    bool publish_progress = false;
    bool mark_finished = false;
    bool mark_success = false;
    std::string final_error;

    {
        std::lock_guard<std::mutex> lock(mtx);
        if(!sync.waiting) {
            return;
        }
        if(sync.terminal_seen && !terminal) {
            return;
        }
        if(sync.last_mcu_status_valid &&
           sync.last_mcu_state == st.state &&
           sync.last_mcu_progress == st.progress) {
            return;
        }
        sync.last_mcu_status_valid = true;
        sync.last_mcu_state = st.state;
        sync.last_mcu_progress = st.progress;

        task_id = sync.task_id;
        component = sync.component;
        if(task_id.empty() || component.empty()) {
            return;
        }

        std::snprintf(p.task_id, sizeof(p.task_id), "%s", task_id.c_str());
        std::snprintf(p.component, sizeof(p.component), "%s", component.c_str());

        switch(state) {
        case middleware::McuOtaState::Success:
            p.state = static_cast<uint8_t>(middleware::OtaProgressState::Completed);
            p.progress_pct = 100;
            p.error_code[0] = '\0';
            publish_progress = true;
            mark_finished = true;
            mark_success = true;
            sync.terminal_seen = true;
            break;
        case middleware::McuOtaState::Cancelled:
            p.state = static_cast<uint8_t>(middleware::OtaProgressState::Failed);
            p.progress_pct = map_apply_progress(st.progress);
            std::snprintf(p.error_code, sizeof(p.error_code), "%s", "cancelled");
            publish_progress = true;
            mark_finished = true;
            final_error = "cancelled";
            sync.terminal_seen = true;
            break;
        case middleware::McuOtaState::Failed:
            p.state = static_cast<uint8_t>(middleware::OtaProgressState::Failed);
            p.progress_pct = map_apply_progress(st.progress);
            std::snprintf(p.error_code, sizeof(p.error_code), "%s",
                          fail_code ? fail_code : "ota_failed");
            publish_progress = true;
            mark_finished = true;
            final_error = fail_code ? fail_code : "ota_failed";
            sync.terminal_seen = true;
            break;
        case middleware::McuOtaState::Preparing:
        case middleware::McuOtaState::Transferring:
        case middleware::McuOtaState::Verifying:
        case middleware::McuOtaState::Idle:
        default: {
            p.state = static_cast<uint8_t>(middleware::OtaProgressState::Applying);
            p.progress_pct = map_apply_progress(st.progress);
            p.error_code[0] = '\0';
            if(!sync.last_apply_pct_valid ||
               p.progress_pct > sync.last_apply_pct) {
                sync.last_apply_pct_valid = true;
                sync.last_apply_pct = p.progress_pct;
                publish_progress = true;
            }
            break;
        }
        }

        if(mark_finished) {
            sync.finished = true;
            sync.success = mark_success;
            sync.error_code = mark_success ? std::string() : final_error;
        }
    }

    if(publish_progress) {
        publish_ota_progress_ui(p);
        (void)publish_ota_progress_cloud(p);
    }
    if(mark_finished) {
        cv.notify_all();
    }
}

void MqttService::on_mcu_ota_status(const middleware::Message& msg)
{
    on_apply_ota_status(msg, galvo_ota_sync_, galvo_ota_mtx_, galvo_ota_cv_,
                        "invalid APP_Mcu_Ota_Status_Ntf payload",
                        "galvo_ota_failed");
}

void MqttService::on_serial_mcu_ota_status(const middleware::Message& msg)
{
    on_apply_ota_status(msg, mcu_ota_sync_, mcu_ota_mtx_, mcu_ota_cv_,
                        "invalid APP_Serial_Mcu_Ota_Status_Ntf payload",
                        "mcu_ota_failed");
}

bool MqttService::trigger_apply_ota(const std::string& task_id,
                                    const std::string& component,
                                    middleware::MsgType start_type,
                                    ApplyOtaSync& sync,
                                    std::mutex& mtx,
                                    const char* already_log,
                                    const char* start_log)
{
    {
        std::lock_guard<std::mutex> lock(mtx);
        if(sync.waiting) {
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "%s task_id=%s",
                                 already_log ? already_log : "ota already waiting",
                                 task_id.c_str());
            return false;
        }
        sync.waiting = true;
        sync.finished = false;
        sync.success = false;
        sync.terminal_seen = false;
        sync.last_apply_pct_valid = false;
        sync.last_apply_pct = 0;
        sync.last_mcu_status_valid = false;
        sync.last_mcu_state = middleware::McuOtaState::Idle;
        sync.last_mcu_progress = 0;
        sync.task_id = task_id;
        sync.component = component;
        sync.error_code.clear();
    }

    middleware::Message start(start_type,
                              middleware::ModuleId::APP_MQTT,
                              middleware::ModuleId::APP_GLAVO);
    (void)middleware::MsgPort::instance().publish_async(start);
    UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info, "%s task_id=%s",
                         start_log ? start_log : "trigger real ota",
                         task_id.c_str());
    return true;
}

bool MqttService::wait_apply_ota(std::string& err_code,
                                 ApplyOtaSync& sync,
                                 std::mutex& mtx,
                                 std::condition_variable& cv,
                                 const char* default_fail)
{
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&sync]() { return sync.finished; });
    const bool ok = sync.success;
    if(ok) {
        err_code.clear();
    } else if(sync.error_code.empty()) {
        err_code = default_fail ? default_fail : "ota_failed";
    } else {
        err_code = sync.error_code;
    }
    sync = ApplyOtaSync{};
    return ok;
}

bool MqttService::trigger_galvo_real_ota(const std::string& task_id, const std::string& component)
{
    return trigger_apply_ota(task_id, component,
                             middleware::MsgType::UI_Mcu_Ota_Start_Req,
                             galvo_ota_sync_, galvo_ota_mtx_,
                             "galvo ota already waiting, reject",
                             "trigger galvo real ota");
}

bool MqttService::wait_galvo_real_ota(std::string& err_code)
{
    return wait_apply_ota(err_code, galvo_ota_sync_, galvo_ota_mtx_, galvo_ota_cv_,
                          "galvo_ota_failed");
}

bool MqttService::trigger_mcu_real_ota(const std::string& task_id, const std::string& component)
{
    return trigger_apply_ota(task_id, component,
                             middleware::MsgType::UI_Serial_Mcu_Ota_Start_Req,
                             mcu_ota_sync_, mcu_ota_mtx_,
                             "mcu ota already waiting, reject",
                             "trigger mcu real ota");
}

bool MqttService::wait_mcu_real_ota(std::string& err_code)
{
    return wait_apply_ota(err_code, mcu_ota_sync_, mcu_ota_mtx_, mcu_ota_cv_,
                          "mcu_ota_failed");
}

void MqttService::start_manifest_download(std::string task_id,
                                          std::vector<OtaComponent> components)
{
    {
        std::lock_guard<std::mutex> lock(ota_mtx_);
        if(ota_download_running_) {
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "ota download already running, ignore task_id=%s",
                                 task_id.c_str());
            return;
        }
        ota_download_running_ = true;
    }
    if(ota_download_thread_.joinable()) {
        ota_download_thread_.join();
    }
    // 下载流程较长，独立线程执行；结束后恢复 running 状态。
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
    // 失败出口统一收敛：同时通知 UI 与云端，保证两侧状态一致。
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

    /* 整目录清空而不是只删本任务的文件：临时文件名带 task_id，掉电中断的任务
     * 会绕过所有清理路径，残留既不会被新任务覆盖也没人删。全包 .img 一次就是
     * 几百 MB，攒几次能把 /userdata 占满，之后所有 OTA 都卡在 no_space 上。
     * 同一时刻只有一个下载任务（ota_download_running_），清空是安全的。 */
    clear_image_tmp_dir();
    if(!ensure_dir(kImageTmpDir)) {
        publish_failed("all", "tmp_dir_failed");
        return;
    }

    std::vector<OtaComponent> ordered_components = std::move(components);
    std::stable_sort(ordered_components.begin(), ordered_components.end(),
                     [](const OtaComponent& a, const OtaComponent& b) {
                         const int ak = (a.component == "soc") ? 1 : 0;
                         const int bk = (b.component == "soc") ? 1 : 0;
                         return ak < bk;
                     });

    bool handled_any_component = false;
    for(const OtaComponent& comp : ordered_components) {
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

        // 云端未提供 filename 时，从 URL 路径推断文件名。
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
        /* 进度节流：原先每读 4KB 就打 UI+MQTT，SOC 全包会变成数十万次上报，
         * 吞吐被进度洪泛拖死。仅在百分比变化，或未知总长时每 500ms 报一次。 */
        uint8_t last_pub_pct = 0xFF;
        auto last_pub_tp = std::chrono::steady_clock::time_point{};
        const bool dl_ok = download_file_with_progress(
            comp.url, tmp_part,
            [this, &progress, &last_pub_pct, &last_pub_tp](size_t recv, size_t total) {
                // 下载阶段仅使用 0~80，后续 90/100 预留给校验与应用。
                uint8_t new_pct = progress.progress_pct;
                if(total > 0) {
                    const int pct = static_cast<int>((recv * 100) / total);
                    new_pct = static_cast<uint8_t>(std::clamp(pct, 0, 80));
                } else if(progress.progress_pct < 80) {
                    new_pct = static_cast<uint8_t>(
                        std::min<int>(80, static_cast<int>(progress.progress_pct) + 1));
                }
                const auto now = std::chrono::steady_clock::now();
                const bool pct_changed = (new_pct != last_pub_pct);
                const bool due_by_time =
                    (total == 0) &&
                    (last_pub_tp.time_since_epoch().count() == 0 ||
                     now - last_pub_tp >= std::chrono::milliseconds(500));
                if(!pct_changed && !due_by_time) {
                    return;
                }
                progress.progress_pct = new_pct;
                last_pub_pct = new_pct;
                last_pub_tp = now;
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

        // SHA256 校验，避免损坏包/篡改包进入应用阶段。
        const std::string local_sha = hex_sha256_file(tmp_final);
        std::string expected_sha = comp.sha256;
        std::transform(expected_sha.begin(), expected_sha.end(), expected_sha.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if(!expected_sha.empty() && local_sha != expected_sha) {
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "sha256_mismatch");
            return;
        }

        /* SOC 全包升级在此分流：包内是单个 .img，不走 AB 对与整目录替换，
         * 落到固定路径后把重启时机交给用户。详见 mqtt_service_soc_ota.cpp。 */
        if(comp.component == "soc") {
            std::string soc_err;
            const bool staged =
                run_soc_ota_stage(task_id, comp, tmp_final, progress, soc_err);
            std::filesystem::remove(tmp_final, ec);
            if(!staged) {
                publish_failed(comp.component.c_str(), soc_err.c_str());
                return;
            }

            /* 固件已就绪：先把进度拉满，再让 UI 切到"是否重启升级"弹窗。 */
            progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Completed);
            progress.progress_pct = 100;
            progress.error_code[0] = '\0';
            publish_ota_progress_ui(progress);
            (void)publish_ota_progress_cloud(progress);

            SocOtaPending pending{};
            if(soc_ota_pending_load(pending)) {
                publish_soc_ota_ready(pending);
            }

            /* 与循环末尾共用段一致：Env Sim 下让心跳立刻反映目标版本。 */
            if(!comp.version.empty()) {
                std::lock_guard<std::mutex> lock(data_mtx_);
                if(env_sim_on_) {
                    soc_fw_override_ = comp.version;
                }
            }
            continue;
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
        for(const std::string& entry_raw : tar_entries) {
            const std::string entry = trim_line(entry_raw);
            if(entry.empty()) {
                continue;
            }
            if(!archive_entry_safe_flat(entry)) {
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "tar_entry_invalid");
                return;
            }
            upc::OtaImageKind kind{};
            if(upc::ota_image_kind_from_component(comp.component, kind)) {
                upc::OtaImageSlot slot = upc::OtaImageSlot::A;
                upc::OtaImageVersion ver{};
                if(!upc::parse_ota_image_basename(entry, kind, slot, ver)) {
                    std::filesystem::remove_all(extract_dir, ec);
                    std::filesystem::remove(tmp_final, ec);
                    publish_failed(comp.component.c_str(), "tar_entry_invalid");
                    return;
                }
            } else if(!is_bin_name(entry)) {
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "tar_entry_invalid");
                return;
            }
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

        std::vector<std::filesystem::path> extracted_files;
        upc::OtaImageKind kind{};
        const bool named_ab = upc::ota_image_kind_from_component(comp.component, kind);
        if(named_ab) {
            std::vector<std::string> extracted_paths;
            for(const auto& ent : std::filesystem::directory_iterator(extract_dir, ec)) {
                if(ec || !ent.is_regular_file()) {
                    continue;
                }
                extracted_paths.push_back(ent.path().string());
            }
            upc::OtaImagePair pair{};
            const upc::OtaImagePairError pair_err =
                upc::select_highest_ota_image_pair(extracted_paths, kind, pair);
            if(extracted_paths.size() != 2 || pair_err != upc::OtaImagePairError::Ok) {
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "ab_pair_invalid");
                return;
            }
            extracted_files.push_back(pair.slot_a_path);
            extracted_files.push_back(pair.slot_b_path);
        } else {
            for(const auto& ent : std::filesystem::directory_iterator(extract_dir, ec)) {
                if(ec || !ent.is_regular_file()) {
                    continue;
                }
                const std::string name = ent.path().filename().string();
                if(!is_bin_name(name)) {
                    continue;
                }
                extracted_files.push_back(ent.path());
            }
            // 解包后要求仅包含两份 bin 文件，避免目录噪声或异常条目。
            if(extracted_files.size() != 2) {
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "extract_ab_invalid");
                return;
            }
        }

        progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Applying);
        progress.progress_pct = static_cast<uint8_t>(
            (comp.component == "galvo" || comp.component == "mcu") ? 90 : 100);
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

        // 先备份老文件，再迁移新文件；迁移失败则尝试回滚。
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

        if(comp.component == "galvo") {
            if(!trigger_galvo_real_ota(task_id, comp.component)) {
                publish_failed(comp.component.c_str(), "galvo_ota_start_failed");
                return;
            }
            std::string apply_err;
            if(!wait_galvo_real_ota(apply_err)) {
                publish_failed(comp.component.c_str(), apply_err.c_str());
                return;
            }
        } else if(comp.component == "mcu") {
            if(!trigger_mcu_real_ota(task_id, comp.component)) {
                publish_failed(comp.component.c_str(), "mcu_ota_start_failed");
                return;
            }
            std::string apply_err;
            if(!wait_mcu_real_ota(apply_err)) {
                publish_failed(comp.component.c_str(), apply_err.c_str());
                return;
            }
        } else {
            progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Completed);
            progress.progress_pct = 100;
            progress.error_code[0] = '\0';
            publish_ota_progress_ui(progress);
            (void)publish_ota_progress_cloud(progress);
        }

        // 在 Env Sim 下可直接更新缓存版本，便于心跳立即反映升级结果。
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
            } else if(comp.component == "galvo" && env_sim_on_) {
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
    for(const OtaComponent& comp : ordered_components) {
        if(comp.component == "mcu") {
            need_mcu_refresh = true;
        } else if(comp.component == "galvo") {
            need_galvo_refresh = true;
        }
    }

    // Env Sim 且 manifest 带 version 时，不强制再向下游模块拉取真实版本。
    if(env_sim_on) {
        for(const OtaComponent& comp : ordered_components) {
            if(comp.component == "mcu" && !comp.version.empty()) {
                need_mcu_refresh = false;
            } else if(comp.component == "galvo" && !comp.version.empty()) {
                need_galvo_refresh = false;
            }
        }
    }

    {
        // 等待窗口：给 MCU/Galvo 异步上报版本一个缓冲时间，再触发心跳发布。
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
    // 兼容 snake_case / camelCase 字段命名。
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
        /* 用 int64 解析：SOC 全包 .img 可超 2GB，int 会截断。键名兼容两种风格。 */
        int64_t size_bytes = 0;
        if(!json_extract_int64(obj, "size_bytes", size_bytes)) {
            (void)json_extract_int64(obj, "sizeBytes", size_bytes);
        }
        if(size_bytes > 0) {
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
