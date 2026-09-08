#include "mqtt_service.hpp"

#include "mqtt_service_internal.hpp"
#include "rs_log.hpp"

#include <cstdio>
#include <utility>

namespace upc::mqtt {

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

    if(cmd_name == kCmdLogCollect) {
        LogCollectCommand collect{};
        if(!parse_log_collect_command(data_json, req_id, collect, reject_reason)) {
            (void)publish_cmd_ack(req_id, kCmdLogCollect, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str(),
                                  ("{\"collectId\":\"" +
                                   json_escape(collect.collect_id) + "\"}").c_str());
            return;
        }
        UPC_LOG_MQTT_SERVICE(Info,
            "MQTT RX log_collect req_id=%s collectId=%s files=%zu",
            collect.req_id.c_str(), collect.collect_id.c_str(),
            collect.topic_files.size());
        const std::string collect_id = collect.collect_id;
        if(!start_log_collect(std::move(collect))) {
            (void)publish_cmd_ack(req_id, kCmdLogCollect, "rejected",
                                  kCmdRejectReasonInvalidState,
                                  ("{\"collectId\":\"" +
                                   json_escape(collect_id) + "\"}").c_str());
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

    middleware::Message msg(middleware::MsgType::UI_Debug_Device_Ntf,
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

} // namespace upc::mqtt
