#include "visual_service.hpp"
#include "cabin_profile.hpp"
#include "rs_log.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace upc::visual {
namespace {
/**
 * 基板读码：先等一帧新快照，再开 timeout_ms 的证据窗口。
 * 空快照不计入「无法判断」。与 Glavo 基板等待（本值 + timeout_ms + 2s）对齐。
 * 内门挡镜头仍从接受请求起按 timeout_ms 计，不走这段。
 */
constexpr uint64_t kCabinPlateNoFrameWaitMs = 8000;

template<class T> bool read_cabin_payload(const middleware::Message& msg, T& out) {
    if(msg.sender_id != middleware::ModuleId::APP_GLAVO ||
       msg.target_id != middleware::ModuleId::APP_VISUAL ||
       msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length != sizeof(T)) return false;
    std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(T)); return true;
}
}

void VisualService::on_cabin_session_start(const middleware::Message& msg) {
    middleware::VisualCabinSessionPayload req{};
    if(!read_cabin_payload(msg,req) || req.session_id == 0) return;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        // A replayed Start cannot revive a stopped/completed session.
        if(!cabin_session_start_allowed(cabin_session_id_,cabin_session_active_ || cabin_check_active_,
                                        cabin_stopped_ms_,req.session_id,monotonic_ms())) return;
        if(first_layer_session_active_ || optical_path_session_active_ || plate_height_session_active_) {
            UPC_LOG_VISUAL_SERVICE(Warn,"cabin inspect start rejected: another visual session active");
            return;
        }
    }
    stop_cabin_session("new_session");
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        cabin_session_id_ = req.session_id; cabin_session_active_ = true;
        cabin_stream_needed_ = true; cabin_idle_deadline_ms_ = monotonic_ms() + 15000;
    }
    cabin_request_ = {};
    cabin_cached_valid_ = false;
    clear_latest_snapshot();
    send_stream_start_req();
    UPC_LOG_VISUAL_SERVICE(Info,"cabin inspect start session=%u",req.session_id);
}

void VisualService::on_cabin_session_stop(const middleware::Message& msg) {
    middleware::VisualCabinSessionPayload req{};
    if(!read_cabin_payload(msg,req)) return;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if(req.session_id == 0 || req.session_id != cabin_session_id_) return;
    }
    stop_cabin_session("glavo_stop");
}

void VisualService::stop_cabin_session(const char* reason) {
    cabin_abort_.store(true);
    if(cabin_thread_.joinable()) cabin_thread_.join();
    cabin_check_active_ = false;
    cabin_cached_valid_ = false;
    {
        std::lock_guard<std::mutex> lock(cabin_result_mtx_);
        cabin_result_pending_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if(cabin_session_active_) cabin_stopped_ms_ = monotonic_ms();
        cabin_session_active_ = false;
        cabin_stream_needed_ = false; cabin_idle_deadline_ms_ = 0;
    }
    release_stream_if_idle(reason);
}

void VisualService::interrupt_cabin_for_legacy() {
    bool active;
    { std::lock_guard<std::mutex> lock(state_mtx_); active = cabin_session_active_; }
    if(!active) return;
    const auto req = cabin_request_;
    const bool reply = cabin_check_active_, door = cabin_check_door_;
    stop_cabin_session("conflicting_visual_session");
    if(reply) {
        middleware::VisualCabinCheckRspPayload rsp{};
        rsp.session_id = req.session_id; rsp.request_id = req.request_id;
        send_cabin_rsp(door,rsp);
    }
}

void VisualService::on_cabin_check_req(const middleware::Message& msg, bool door) {
    middleware::VisualCabinCheckReqPayload req{};
    if(!read_cabin_payload(msg,req)) return;
    // Complete the previous pending reply before deciding whether this is a duplicate.
    drain_cabin_result();
    bool active;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        active = cabin_session_active_ && cabin_session_id_ == req.session_id &&
            !first_layer_session_active_ && !optical_path_session_active_ && !plate_height_session_active_;
    }
    middleware::VisualCabinCheckRspPayload unknown{};
    unknown.session_id = req.session_id; unknown.request_id = req.request_id;
    if(!active || !req.session_id || !req.request_id || req.mode > 1 ||
       (!door && req.mode != 0) || req.reserved[0] || req.reserved[1] ||
       req.cabin_led_mask != 0x3f ||
       req.timeout_ms < 250 || req.timeout_ms > 60000) {
        send_cabin_rsp(door,unknown); return;
    }
    if(cabin_cached_valid_ && req.request_id == cabin_cached_rsp_.request_id && door == cabin_cached_door_) {
        // An ID identifies the full immutable request, not just its operation.
        send_cabin_rsp(door, std::memcmp(&req, &cabin_request_, sizeof(req)) == 0
                               ? cabin_cached_rsp_ : unknown);
        return;
    }
    if(cabin_check_active_) {
        if(req.request_id == cabin_request_.request_id && door == cabin_check_door_ &&
           std::memcmp(&req, &cabin_request_, sizeof(req)) == 0) return;
        send_cabin_rsp(door,unknown); return; // no parallel tasks; do not replace active request
    }
    if(cabin_request_.request_id && static_cast<int32_t>(req.request_id-cabin_request_.request_id) <= 0) {
        send_cabin_rsp(door,unknown); return;
    }
    if(cabin_thread_.joinable()) cabin_thread_.join();
    cabin_request_ = req; cabin_check_door_ = door; cabin_check_active_ = true;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        cabin_stream_needed_ = true;
    }
    send_stream_start_req();
    cabin_abort_.store(false);
    uint32_t baseline = 0;
    { std::lock_guard<std::mutex> lock(frame_mtx_); baseline = latest_frame_seq_; }
    UPC_LOG_VISUAL_SERVICE(Info,"cabin inspect running kind=%s session=%u request=%u timeout_ms=%u mode=%u",
        door ? "door" : "plate",req.session_id,req.request_id,req.timeout_ms,
        static_cast<unsigned>(req.mode));
    cabin_thread_ = std::thread([this,req,door,baseline] { run_cabin_window(req,door,baseline); });
}

void VisualService::run_cabin_window(middleware::VisualCabinCheckReqPayload req,
                                    bool door, uint32_t baseline) {
    const uint64_t start = monotonic_ms();
    uint64_t deadline = start + req.timeout_ms;
    middleware::VisualCabinCheckRspPayload rsp{};
    rsp.session_id = req.session_id; rsp.request_id = req.request_id;
    std::string reason = "timeout_or_no_fresh_frame";
    double score = 0;
    std::string profile_source;
    const bool config_ok = load_cabin_profile(cabin_detector_,door,
        std::getenv(door ? "UPC_CABIN_DOOR_CONFIG" : "UPC_CABIN_PLATE_CONFIG"),
        door ? "/userdata/visual/cabin_door_config.json" : "/userdata/visual/cabin_plate_config.json",
        profile_source,reason);
    UPC_LOG_VISUAL_SERVICE(Info,"cabin profile session=%u request=%u source=%s valid=%u",
        req.session_id,req.request_id,profile_source.c_str(),static_cast<unsigned>(config_ok));
    if(config_ok) reason = "timeout_or_no_fresh_frame";
    uint32_t last_seq = baseline;
    uint64_t evidence_start = start;
    /* 基板：出第一帧之前不跑证据、不消耗 timeout_ms。整段无新帧直接 Unknown。 */
    if(config_ok && !door) {
        const uint64_t frame_deadline = monotonic_ms() + kCabinPlateNoFrameWaitMs;
        bool saw_fresh = false;
        uint64_t last_nudge_ms = 0;
        while(!cabin_abort_.load() && monotonic_ms() < frame_deadline) {
            std::vector<uint8_t> nv12;
            uint32_t w = 0, h = 0, seq = 0;
            if(copy_latest_snapshot(nv12, w, h, seq) && seq != 0 && seq != baseline) {
                saw_fresh = true;
                break;
            }
            const uint64_t now = monotonic_ms();
            if(!stream_is_serving() &&
               (last_nudge_ms == 0 || now - last_nudge_ms >= 500)) {
                last_nudge_ms = now;
                send_stream_start_req();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if(cabin_abort_.load()) return;
        if(!saw_fresh) {
            reason = "no_frame";
            {
                std::lock_guard<std::mutex> lock(cabin_result_mtx_);
                cabin_pending_rsp_ = rsp;
                cabin_result_pending_ = true;
            }
            UPC_LOG_VISUAL_SERVICE(Warn,
                "cabin inspect plate qr session=%u request=%u result=0 seq=0 reason=no_frame",
                req.session_id, req.request_id);
            return;
        }
        evidence_start = monotonic_ms();
        deadline = evidence_start + req.timeout_ms;
    }
    CabinEvidence evidence(door, req.mode == 1, baseline, evidence_start);
    CabinPlateSceneHold plate_hold;
    uint8_t raw_candidate = 0;
    uint64_t last_inference_ms = 0;
    while(config_ok && !cabin_abort_.load() && monotonic_ms() < deadline) {
        std::vector<uint8_t> nv12; uint32_t w = 0, h = 0, seq = 0;
        if(copy_latest_snapshot(nv12,w,h,seq) && seq != last_seq) {
            last_seq = seq;
            CabinDetection d;
            if(!door && plate_hold.active() &&
               plate_hold.stable(nv12.data(),nv12.size(),w,h)) {
                d = {2,score,"codes_absent_scene_stable"};
                last_inference_ms = 0;
            } else {
                if(plate_hold.active()) {
                    plate_hold.clear();
                    evidence.break_streak();
                }
                const uint64_t inference_start = monotonic_ms();
                d = door ? cabin_detector_.door(nv12.data(),nv12.size(),w,h,req.cabin_led_mask)
                         : cabin_detector_.plate(nv12.data(),nv12.size(),w,h,req.cabin_led_mask);
                last_inference_ms = monotonic_ms() - inference_start;
                if(!door && d.result == 2)
                    plate_hold.capture(nv12.data(),nv12.size(),w,h);
            }
            const uint64_t now = monotonic_ms();
            reason = d.reason;
            score = d.score;
            raw_candidate = d.result;
            if(now < deadline) rsp.result = evidence.observe(d.result,seq,now);
            rsp.frame_seq = seq;
            if(rsp.result != 0) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if(cabin_abort_.load()) return;
    // Enqueue only; the IPC thread validates session/cancellation before publishing.
    {
        std::lock_guard<std::mutex> lock(cabin_result_mtx_);
        cabin_pending_rsp_ = rsp; cabin_result_pending_ = true;
    }
    UPC_LOG_VISUAL_SERVICE(Info,"cabin inspect %s session=%u request=%u result=%u raw=%u seq=%u score=%.3f inference_ms=%llu elapsed_ms=%llu reason=%s",
        door ? "inner door occl" : "plate qr",req.session_id,req.request_id,
        static_cast<unsigned>(rsp.result),static_cast<unsigned>(raw_candidate),rsp.frame_seq,score,
        static_cast<unsigned long long>(last_inference_ms),
        static_cast<unsigned long long>(monotonic_ms()-start),reason.c_str());
}

void VisualService::drain_cabin_result() {
    bool idle_expired;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        idle_expired = cabin_session_active_ && !cabin_check_active_ &&
            cabin_idle_deadline_ms_ && monotonic_ms() >= cabin_idle_deadline_ms_;
    }
    if(idle_expired) { stop_cabin_session("idle_lease_expired"); return; }
    middleware::VisualCabinCheckRspPayload rsp{};
    {
        std::lock_guard<std::mutex> lock(cabin_result_mtx_);
        if(!cabin_result_pending_) return;
        rsp = cabin_pending_rsp_; cabin_result_pending_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if(!cabin_session_active_ || cabin_session_id_ != rsp.session_id ||
           !cabin_check_active_ || cabin_request_.request_id != rsp.request_id) return;
    }
    if(cabin_thread_.joinable()) cabin_thread_.join();
    cabin_check_active_ = false; // detection stops BEFORE the reply is sent
    cabin_cached_valid_ = true; cabin_cached_door_ = cabin_check_door_; cabin_cached_rsp_ = rsp;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        cabin_stream_needed_ = false; cabin_idle_deadline_ms_ = monotonic_ms() + 15000;
    }
    release_stream_if_idle("cabin_round_complete");
    send_cabin_rsp(cabin_check_door_,rsp);
}

void VisualService::send_cabin_rsp(bool door, const middleware::VisualCabinCheckRspPayload& rsp) {
    middleware::Message out(door ? middleware::MsgType::APP_Visual_InnerDoor_Occlusion_Rsp
                                : middleware::MsgType::APP_Visual_Plate_Present_Rsp,
                           middleware::ModuleId::APP_VISUAL,middleware::ModuleId::APP_GLAVO);
    out.payload = middleware::MsgPayload::from_bytes(reinterpret_cast<const uint8_t*>(&rsp),sizeof(rsp));
    if(!middleware::MsgPort::instance().publish_async(out))
        UPC_LOG_VISUAL_SERVICE(Warn,"cabin inspect reply publish failed session=%u request=%u",rsp.session_id,rsp.request_id);
}
} // namespace upc::visual
