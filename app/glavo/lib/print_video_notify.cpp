#include "print_video_notify.hpp"

#include "device_config.hpp"
#include "msg_port.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace upc::glavo {

namespace {

void copy_job_basename(const std::string& job_path, char* out, size_t out_len)
{
    if(!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if(job_path.empty()) {
        std::snprintf(out, out_len, "print");
        return;
    }
    const size_t slash = job_path.find_last_of('/');
    const char* base =
        slash == std::string::npos ? job_path.c_str()
                                   : job_path.c_str() + slash + 1;
    std::snprintf(out, out_len, "%s", base);
    char* dot = std::strrchr(out, '.');
    if(dot) {
        *dot = '\0';
    }
    if(out[0] == '\0') {
        std::snprintf(out, out_len, "print");
    }
}

void make_session_id(char* out, size_t out_len)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    std::snprintf(out, out_len, "pv%lld", static_cast<long long>(ms));
}

} // namespace

PrintVideoNotify& PrintVideoNotify::instance()
{
    static PrintVideoNotify inst;
    return inst;
}

void PrintVideoNotify::set_print_h264_continuous(bool on)
{
    print_h264_continuous_on_ = on;
}

void PrintVideoNotify::reset_session_state()
{
    active_        = false;
    layer_index_   = 0;
    session_id_[0] = '\0';
    job_name_[0]   = '\0';
}

bool PrintVideoNotify::publish_to_camera(middleware::MsgType type,
                                         const void* payload, size_t len)
{
    middleware::Message msg(type,
                            middleware::ModuleId::APP_GLAVO,
                            middleware::ModuleId::APP_CAMERA);
    msg.payload = middleware::MsgPayload::from_bytes(
        static_cast<const uint8_t*>(payload), len);
    if(!middleware::MsgPort::instance().publish_async(msg)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "print_video publish fail type=%u session=%s",
                              static_cast<unsigned>(type), session_id_);
        return false;
    }
    return true;
}

void PrintVideoNotify::on_print_start(const std::string& job_path)
{
#if !GLAVO_ENABLE_LAYER_SNAP
    (void)job_path;
    return;
#endif
    reset_session_state();
    make_session_id(session_id_, sizeof(session_id_));
    copy_job_basename(job_path, job_name_, sizeof(job_name_));
    layer_index_ = 0;
    active_      = true;

    middleware::PrintVideoSessionPayload p{};
    std::snprintf(p.session_id, sizeof(p.session_id), "%s", session_id_);
    std::snprintf(p.job_name, sizeof(p.job_name), "%s", job_name_);
    p.h264_continuous_on = print_h264_continuous_on_ ? 1 : 0;

    publish_to_camera(middleware::MsgType::APP_Print_Video_Session_Start_Ntf,
                      &p, sizeof(p));
    UPC_LOG_GLAVO_SERVICE(Info, "print_video session start id=%s job=%s h264=%d",
                          session_id_, job_name_,
                          print_h264_continuous_on_ ? 1 : 0);
}

void PrintVideoNotify::on_layer_snap(uint16_t layer_height_um)
{
#if !GLAVO_ENABLE_LAYER_SNAP
    (void)layer_height_um;
    return;
#endif
    if(!active_) {
        return;
    }

    ++layer_index_;
    middleware::PrintVideoLayerSnapPayload p{};
    std::snprintf(p.session_id, sizeof(p.session_id), "%s", session_id_);
    p.layer_index     = layer_index_;
    p.layer_height_um = layer_height_um;

    if(publish_to_camera(middleware::MsgType::APP_Print_Layer_Snap_Ntf,
                          &p, sizeof(p))) {
        UPC_LOG_GLAVO_SERVICE(Debug,
            "print_video layer_snap session=%s layer=%u height_um=%u",
            session_id_, layer_index_,
            static_cast<unsigned>(layer_height_um));
    }
}

void PrintVideoNotify::on_session_end(middleware::MsgType type, const char* log_tag)
{
#if !GLAVO_ENABLE_LAYER_SNAP
    (void)type;
    (void)log_tag;
    return;
#endif
    if(!active_) {
        return;
    }

    middleware::PrintVideoSessionPayload p{};
    std::snprintf(p.session_id, sizeof(p.session_id), "%s", session_id_);
    std::snprintf(p.job_name, sizeof(p.job_name), "%s", job_name_);
    publish_to_camera(type, &p, sizeof(p));
    UPC_LOG_GLAVO_SERVICE(Info, "print_video session %s id=%s layers=%u",
                          log_tag, session_id_, layer_index_);
    reset_session_state();
}

void PrintVideoNotify::on_print_finish()
{
    on_session_end(middleware::MsgType::APP_Print_Video_Session_Finish_Ntf,
                   "finish");
}

void PrintVideoNotify::on_print_abort()
{
    on_session_end(middleware::MsgType::APP_Print_Video_Session_Abort_Ntf,
                   "abort");
}

} // namespace upc::glavo
