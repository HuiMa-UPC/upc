#include "media_service.hpp"

#include "config/cal_config.hpp"
#include "device_config.hpp"
#include "print_history_manifest.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <filesystem>
#include <unordered_set>

namespace upc::media {

namespace {

constexpr const char* kPhotoOutDir = "/home/photo";
constexpr uint64_t kStartRetryMs = 1000;

std::string sanitize_token_for_dir(const char* s)
{
    if(!s || !s[0]) {
        return {};
    }
    std::string out(s);
    for(char& c : out) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if(!std::isalnum(uc) && c != '_' && c != '-' && c != '.') {
            c = '_';
        }
    }
    return out;
}

std::string build_work_dir_name_for_session(const char* job_name,
                                            const char* session_id)
{
    std::string session_name = sanitize_token_for_dir(job_name);
    if(session_name.empty()) {
        session_name = "print";
    }
    const std::string token = sanitize_token_for_dir(session_id);
    return session_name + ".__work_" + (token.empty() ? "session" : token);
}

} // namespace

MediaService::~MediaService()
{
    shutdown();
}

bool MediaService::init()
{
    cleanup_stale_work_dirs_on_startup();
    subscribe_messages();
    print_video_.set_sender_module_id(middleware::ModuleId::APP_MEDIA);
    print_video_.set_nv12_snapshot_provider([this](std::vector<uint8_t>& out) {
        return copy_latest_nv12_snapshot(out);
    });

    uint32_t timelapse_fps = CAMERA_PRINT_VIDEO_LAYER_FPS;
    upc::config::CalConfigOptions cal_cfg{};
    if(upc::config::load_cal_config_options(upc::config::kCalDefaultPath, &cal_cfg) &&
       cal_cfg.has_config_section && cal_cfg.has_timelapse_video_fps &&
       cal_cfg.timelapse_video_fps > 0) {
        timelapse_fps = cal_cfg.timelapse_video_fps;
    }
    print_video_.set_timelapse_fps(timelapse_fps);
    clean_video_.set_timelapse_fps(timelapse_fps);
    UPC_LOG_MEDIA_SERVICE(Info, "timelapse_video_fps=%u", timelapse_fps);

    send_stream_start_req();
    return true;
}

void MediaService::shutdown()
{
    send_stream_stop_req();
    stop_worker();
    consumer_.close();
    unmap_dma_views();
    close_dma_fds();
    subs_.clear();
}

void MediaService::tick()
{
    const uint64_t now = monotonic_ms();
    std::lock_guard<std::mutex> lock(state_mtx_);
    if(stream_up_) {
        return;
    }
    if(start_req_sent_ && now - last_start_req_ms_ < kStartRetryMs) {
        return;
    }
    send_stream_start_req();
}

void MediaService::subscribe_messages()
{
    auto& bus = middleware::MsgPort::instance();

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Media_Camera_Stream_Start_Rsp,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { on_stream_start_rsp(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Media_Camera_Stream_Stop_Done,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { on_stream_stop_done(msg); }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::UI_Camera_Snapshot_Req,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Video_Session_Start_Ntf,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Layer_Snap_Ntf,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Video_Session_Finish_Ntf,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_Video_Session_Abort_Ntf,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Print_H264_Record_Pause_Ntf,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Clean_Video_Session_Start_Ntf,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Clean_Video_Session_Stop_Ntf,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::UI_Print_Video_Finalize_Req,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::UI_Storage_Usage_Req,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
}

void MediaService::handle_message(const middleware::Message& msg)
{
    switch(msg.type) {
    case middleware::MsgType::UI_Camera_Snapshot_Req:
        handle_snapshot_req(msg);
        break;
    case middleware::MsgType::APP_Print_Video_Session_Start_Ntf:
    case middleware::MsgType::APP_Print_Layer_Snap_Ntf:
    case middleware::MsgType::APP_Print_Video_Session_Finish_Ntf:
    case middleware::MsgType::APP_Print_Video_Session_Abort_Ntf:
        handle_print_video(msg);
        break;
    case middleware::MsgType::APP_Print_H264_Record_Pause_Ntf: {
        middleware::PrintPauseStatePayload p{};
        if(read_inline_pod(msg.payload, p)) {
            print_h264_.set_paused(p.paused != 0);
        }
        break;
    }
    case middleware::MsgType::APP_Clean_Video_Session_Start_Ntf:
    case middleware::MsgType::APP_Clean_Video_Session_Stop_Ntf:
        handle_clean_video(msg);
        break;
    case middleware::MsgType::UI_Print_Video_Finalize_Req:
        handle_print_video_finalize(msg);
        break;
    case middleware::MsgType::UI_Storage_Usage_Req:
        handle_storage_usage_req(msg);
        break;
    default:
        break;
    }
}

void MediaService::send_stream_start_req()
{
    middleware::Message msg(
        middleware::MsgType::MEDIA_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_MEDIA,
        middleware::ModuleId::APP_CAMERA);
    middleware::MsgPort::instance().publish_async(msg);
    start_req_sent_ = true;
    last_start_req_ms_ = monotonic_ms();
}

void MediaService::send_stream_stop_req()
{
    middleware::Message msg(
        middleware::MsgType::MEDIA_Camera_Stream_Stop_Req,
        middleware::ModuleId::APP_MEDIA,
        middleware::ModuleId::APP_CAMERA);
    middleware::MsgPort::instance().publish_async(msg);
}

bool MediaService::read_inline_start_rsp(const middleware::Message& msg,
                                         middleware::CameraStreamInfoPayload& out)
{
    if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length < sizeof(out)) {
        return false;
    }
    std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(out));
    return true;
}

void MediaService::on_stream_start_rsp(const middleware::Message& msg)
{
    middleware::CameraStreamInfoPayload info{};
    if(!read_inline_start_rsp(msg, info)) {
        return;
    }
    info.shm_name[sizeof(info.shm_name) - 1] = '\0';

    stop_worker();
    consumer_.close();
    unmap_dma_views();
    close_dma_fds();

    if(info.ok == 0 || info.dma_mode == 0) {
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }

    std::vector<int> fds;
    if(!middleware::MsgPort::instance().take_peer_dma_fds(
           middleware::ModuleId::APP_CAMERA, fds)) {
        UPC_LOG_MEDIA_SERVICE(Error, "take dma fds failed");
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }
    if(fds.size() != info.dma_buf_count) {
        for(int fd : fds) {
            if(fd >= 0) {
                ::close(fd);
            }
        }
        UPC_LOG_MEDIA_SERVICE(Error,
            "media dma fd count mismatch got=%zu expected=%u",
            fds.size(), info.dma_buf_count);
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }
    if(consumer_.open(info.shm_name) != upc_ipc::IpcShmError::OK) {
        for(int fd : fds) {
            if(fd >= 0) {
                ::close(fd);
            }
        }
        UPC_LOG_MEDIA_SERVICE(Error, "open shm failed %s", info.shm_name);
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }

    dma_fds_ = std::move(fds);
    stream_info_ = info;
    stream_width_ = info.width;
    stream_height_ = info.height;
    stream_stride_y_ = info.stride_y;
    stream_fps_ = info.fps;

    print_video_.set_stream_geometry(stream_width_, stream_height_);
    print_h264_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
    clean_video_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);

    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = true;
        start_req_sent_ = false;
    }

    stop_worker_.store(false);
    worker_ = std::thread(&MediaService::worker_thread_fn, this);
}

void MediaService::on_stream_stop_done(const middleware::Message& msg)
{
    (void)msg;
    std::lock_guard<std::mutex> lock(state_mtx_);
    stream_up_ = false;
    start_req_sent_ = false;
}

void MediaService::stop_worker()
{
    stop_worker_.store(true);
    if(worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(state_mtx_);
    stream_up_ = false;
}

void MediaService::close_dma_fds()
{
    for(int fd : dma_fds_) {
        if(fd >= 0) {
            ::close(fd);
        }
    }
    dma_fds_.clear();
}

void MediaService::unmap_dma_views()
{
    if(dma_map_size_ == 0) {
        dma_maps_.clear();
        return;
    }
    for(auto* ptr : dma_maps_) {
        if(ptr) {
            ::munmap(ptr, dma_map_size_);
        }
    }
    dma_maps_.clear();
    dma_map_size_ = 0;
}

bool MediaService::map_dma_views_if_needed()
{
    if(!dma_maps_.empty()) {
        return true;
    }
    if(dma_fds_.empty() || stream_stride_y_ == 0 || stream_height_ == 0) {
        return false;
    }
    dma_map_size_ = static_cast<size_t>(stream_stride_y_) * stream_height_ * 3u / 2u;
    dma_maps_.resize(dma_fds_.size(), nullptr);
    for(size_t i = 0; i < dma_fds_.size(); ++i) {
        void* p = ::mmap(nullptr, dma_map_size_, PROT_READ, MAP_SHARED, dma_fds_[i], 0);
        if(p == MAP_FAILED) {
            UPC_LOG_MEDIA_SERVICE(Error, "mmap dma fd failed idx=%zu", i);
            unmap_dma_views();
            return false;
        }
        dma_maps_[i] = static_cast<uint8_t*>(p);
    }
    return true;
}

void MediaService::worker_thread_fn()
{
    while(!stop_worker_.load(std::memory_order_relaxed)) {
        auto frame = consumer_.acquire_latest_frame();
        if(!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if(frame->buffer_idx < dma_fds_.size() &&
           map_dma_views_if_needed()) {
            const uint32_t width = frame->width();
            const uint32_t height = frame->height();
            const uint32_t stride = frame->stride_y();
            if(width > 0 && height > 0 && stride > 0 &&
               frame->buffer_idx < dma_maps_.size()) {
                uint8_t* src = dma_maps_[frame->buffer_idx];
                if(src) {
                    const uint8_t* src_y = src;
                    const uint8_t* src_uv =
                        src + static_cast<size_t>(stride) * height;
                    /* 连续录像/清粉录像逐帧入口：已迁移到 media 进程。 */
                    clean_video_.on_frame_nv12(src_y, src_uv, stride);
                    print_h264_.on_frame_nv12(src_y, src_uv, stride);
                }
            }
            update_latest_nv12_snapshot(*frame);
        }
        consumer_.release_slot(frame->slot_index);
    }
}

void MediaService::copy_nv12_tight(uint8_t* dst, const uint8_t* src_y,
                                   const uint8_t* src_uv, uint32_t src_stride_y,
                                   uint32_t width, uint32_t height)
{
    if(!dst || !src_y || !src_uv || src_stride_y == 0 || width == 0 || height == 0) {
        return;
    }
    const size_t y_size = static_cast<size_t>(width) * height;
    uint8_t* dst_uv = dst + y_size;
    if(src_stride_y == width) {
        std::memcpy(dst, src_y, y_size);
        std::memcpy(dst_uv, src_uv, y_size / 2u);
        return;
    }
    for(uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * width,
                    src_y + static_cast<size_t>(row) * src_stride_y,
                    width);
    }
    for(uint32_t row = 0; row < height / 2u; ++row) {
        std::memcpy(dst_uv + static_cast<size_t>(row) * width,
                    src_uv + static_cast<size_t>(row) * src_stride_y,
                    width);
    }
}

void MediaService::update_latest_nv12_snapshot(const upc_ipc::IpcDmaFrameView& frame)
{
    if(frame.buffer_idx >= dma_maps_.size()) {
        return;
    }
    uint8_t* src = dma_maps_[frame.buffer_idx];
    if(!src) {
        return;
    }
    const uint32_t width = frame.width();
    const uint32_t height = frame.height();
    const uint32_t stride = frame.stride_y();
    if(width == 0 || height == 0 || stride == 0) {
        return;
    }

    const uint8_t* src_y = src;
    const uint8_t* src_uv = src + static_cast<size_t>(stride) * height;
    const size_t need =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3u / 2u;
    std::lock_guard<std::mutex> lock(latest_nv12_mtx_);
    latest_nv12_snapshot_.resize(need);
    copy_nv12_tight(latest_nv12_snapshot_.data(), src_y, src_uv, stride,
                    width, height);
    latest_nv12_valid_ = true;
}

bool MediaService::copy_latest_nv12_snapshot(std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lock(latest_nv12_mtx_);
    if(!latest_nv12_valid_ || latest_nv12_snapshot_.empty()) {
        return false;
    }
    out = latest_nv12_snapshot_;
    return true;
}

void MediaService::handle_snapshot_req(const middleware::Message& msg)
{
    std::string out_path;
    const bool ok = save_latest_png_snapshot(out_path);
    send_snapshot_rsp(msg, ok, ok ? out_path.c_str() : "");
}

bool MediaService::save_latest_png_snapshot(std::string& out_path) const
{
    out_path.clear();
    std::vector<uint8_t> nv12;
    if(!copy_latest_nv12_snapshot(nv12) || nv12.empty() ||
       stream_width_ == 0 || stream_height_ == 0) {
        return false;
    }

    if(mkdir(kPhotoOutDir, 0755) != 0 && errno != EEXIST) {
        UPC_LOG_MEDIA_SERVICE(Error, "mkdir %s failed: %s",
                              kPhotoOutDir, std::strerror(errno));
        return false;
    }

    const std::string stamp = format_stamp_local();
    const std::string name = "photo_" + stamp + "_" +
                             std::to_string(stream_width_) + "x" +
                             std::to_string(stream_height_) + ".png";
    const std::string out_png_path = std::string(kPhotoOutDir) + "/" + name;
    const std::string tmp_nv12_path =
        "/tmp/media_snapshot_" + stamp + "_" + std::to_string(monotonic_us()) + ".nv12";

    FILE* fp = std::fopen(tmp_nv12_path.c_str(), "wb");
    if(!fp) {
        return false;
    }
    const bool ok = std::fwrite(nv12.data(), 1, nv12.size(), fp) == nv12.size();
    if(std::fclose(fp) != 0 || !ok) {
        (void)std::remove(tmp_nv12_path.c_str());
        return false;
    }

    char cmd[1024];
    std::snprintf(
        cmd, sizeof(cmd),
        "%s -y -loglevel error -f rawvideo -pix_fmt nv12 -s %ux%u -i '%s' "
        "-frames:v 1 -c:v png '%s'",
        CAMERA_FFMPEG_BIN, stream_width_, stream_height_,
        tmp_nv12_path.c_str(), out_png_path.c_str());
    const bool png_ok = (run_shell_cmd(cmd, "ffmpeg png") == 0);
    (void)std::remove(tmp_nv12_path.c_str());
    if(!png_ok) {
        return false;
    }
    out_path = out_png_path;
    UPC_LOG_MEDIA_SERVICE(Info, "snapshot saved path=%s", out_path.c_str());
    return true;
}

void MediaService::send_snapshot_rsp(const middleware::Message& req, bool ok,
                                     const char* path) const
{
    middleware::CameraSnapshotRspPayload rsp_payload{};
    rsp_payload.ok = ok ? 1 : 0;
    if(path && path[0]) {
        std::snprintf(rsp_payload.path, sizeof(rsp_payload.path), "%s", path);
    }

    middleware::Message rsp(
        middleware::MsgType::APP_Camera_Snapshot_Rsp,
        middleware::ModuleId::APP_MEDIA,
        req.sender_id);
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp_payload), sizeof(rsp_payload));
    middleware::MsgPort::instance().publish_async(rsp);
}

void MediaService::handle_print_video(const middleware::Message& msg)
{
    switch(msg.type) {
    case middleware::MsgType::APP_Print_Video_Session_Start_Ntf: {
        middleware::PrintVideoSessionPayload info{};
        if(!read_inline_pod(msg.payload, info)) {
            return;
        }
        info.session_id[sizeof(info.session_id) - 1] = '\0';
        info.job_name[sizeof(info.job_name) - 1] = '\0';
        cleanup_work_dirs_before_print_start(info);
        {
            std::lock_guard<std::mutex> lock(finalize_mtx_);
            latest_print_session_closed_ = false;
        }
        print_video_.set_stream_geometry(stream_width_, stream_height_);
        print_h264_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
        print_video_.on_session_start(info);
        print_h264_.on_session_start(info);
        break;
    }
    case middleware::MsgType::APP_Print_Layer_Snap_Ntf: {
        middleware::PrintVideoLayerSnapPayload snap{};
        if(!read_inline_pod(msg.payload, snap)) {
            return;
        }
        print_video_.on_layer_snap(snap);
        break;
    }
    case middleware::MsgType::APP_Print_Video_Session_Finish_Ntf:
    case middleware::MsgType::APP_Print_Video_Session_Abort_Ntf: {
        middleware::PrintVideoSessionPayload info{};
        if(!read_inline_pod(msg.payload, info)) {
            return;
        }
        info.session_id[sizeof(info.session_id) - 1] = '\0';
        info.job_name[sizeof(info.job_name) - 1] = '\0';
        {
            std::lock_guard<std::mutex> lock(finalize_mtx_);
            latest_print_session_id_ = info.session_id;
            latest_print_job_name_ = info.job_name;
            latest_print_session_closed_ = true;
        }
        if(msg.type == middleware::MsgType::APP_Print_Video_Session_Finish_Ntf) {
            print_h264_.on_session_finish(info);
            print_video_.on_session_finish(info);
        } else {
            print_h264_.on_session_abort(info);
            print_video_.on_session_abort(info);
        }
        break;
    }
    default:
        break;
    }
}

void MediaService::handle_clean_video(const middleware::Message& msg)
{
    if(msg.type == middleware::MsgType::APP_Clean_Video_Session_Start_Ntf) {
        middleware::CleanVideoSessionStartPayload req{};
        if(!read_inline_pod(msg.payload, req)) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(finalize_mtx_);
            clean_video_active_ = true;
        }
        clean_video_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
        clean_video_.on_session_start(req);
        return;
    }
    if(msg.type == middleware::MsgType::APP_Clean_Video_Session_Stop_Ntf) {
        middleware::CleanVideoSessionStopPayload req{};
        if(!read_inline_pod(msg.payload, req)) {
            return;
        }
        clean_video_.on_session_stop(req);

        std::string finalize_frames_dir;
        {
            std::lock_guard<std::mutex> lock(finalize_mtx_);
            clean_video_active_ = false;
            if(finalize_pending_ && !pending_finalize_frames_dir_.empty()) {
                finalize_frames_dir = pending_finalize_frames_dir_;
                finalize_pending_ = false;
                pending_finalize_frames_dir_.clear();
            }
        }
        if(!finalize_frames_dir.empty()) {
            print_video_.finalize_after_exit(finalize_frames_dir.c_str());
        }
    }
}

void MediaService::handle_print_video_finalize(const middleware::Message& msg)
{
    middleware::PrintVideoFinalizeReqPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        return;
    }
    req.frames_dir[sizeof(req.frames_dir) - 1] = '\0';
    if(req.frames_dir[0] == '\0') {
        std::string session_id;
        std::string job_name;
        bool can_cleanup = false;
        {
            std::lock_guard<std::mutex> lock(finalize_mtx_);
            can_cleanup = latest_print_session_closed_;
            if(can_cleanup) {
                session_id = latest_print_session_id_;
                job_name = latest_print_job_name_;
                latest_print_session_closed_ = false;
            }
        }
        if(can_cleanup && !session_id.empty() && !job_name.empty()) {
            print_h264_.enqueue_cleanup_for_session(job_name.c_str(), session_id.c_str());
        } else {
            UPC_LOG_MEDIA_SERVICE(
                Warn, "print_h264 cleanup skipped: stale or empty finalize request");
        }
        return;
    }

    (void)print_video_.commit_after_exit(req.frames_dir);

    bool defer = false;
    {
        std::lock_guard<std::mutex> lock(finalize_mtx_);
        if(clean_video_active_) {
            defer = true;
            finalize_pending_ = true;
            pending_finalize_frames_dir_ = req.frames_dir;
        }
    }
    if(!defer) {
        print_video_.finalize_after_exit(req.frames_dir);
    }
}

void MediaService::handle_storage_usage_req(const middleware::Message& msg) const
{
    struct statvfs vfs {};
    middleware::StorageUsagePayload rsp{};
    if(::statvfs(CAMERA_PRINT_VIDEO_OUT_DIR, &vfs) == 0) {
        const uint64_t block_size = static_cast<uint64_t>(vfs.f_frsize);
        const uint64_t total = static_cast<uint64_t>(vfs.f_blocks) * block_size;
        const uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * block_size;
        rsp.total_bytes = total;
        rsp.used_bytes = (total > free_bytes) ? (total - free_bytes) : 0u;
    } else {
        UPC_LOG_MEDIA_SERVICE(Warn, "storage statvfs failed path=%s err=%d",
                              CAMERA_PRINT_VIDEO_OUT_DIR, errno);
        rsp.total_bytes = 0;
        rsp.used_bytes = 0;
    }

    middleware::Message ntf(
        middleware::MsgType::APP_Storage_Usage_Rsp,
        middleware::ModuleId::APP_MEDIA,
        msg.sender_id);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    middleware::MsgPort::instance().publish_async(ntf);
}

uint64_t MediaService::monotonic_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t MediaService::monotonic_us()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string MediaService::format_stamp_local()
{
    const auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    if(localtime_r(&t, &tm_buf) == nullptr) {
        return "19700101_000000_000";
    }
    const uint32_t ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d_%03u",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, ms);
    return buf;
}

int MediaService::run_shell_cmd(const char* cmd, const char* log_tag)
{
    if(!cmd || !cmd[0]) {
        return -1;
    }
    UPC_LOG_MEDIA_SERVICE(Info, "%s: %s", log_tag ? log_tag : "cmd", cmd);
    const int rc = std::system(cmd);
    if(rc != 0) {
        UPC_LOG_MEDIA_SERVICE(Error, "%s failed rc=%d",
                              log_tag ? log_tag : "cmd", rc);
    }
    return rc;
}

void MediaService::cleanup_stale_work_dirs_on_startup() const
{
    std::vector<upc::PrintHistoryEntry> entries;
    (void)upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries);

    std::unordered_set<std::string> keep_frames_dirs;
    for(const auto& e : entries) {
        /* 仍在进行中的会话目录不清理：未提交或视频仍在后台生成。 */
        if((!e.committed || e.video_pending) && !e.frames_dir.empty()) {
            keep_frames_dirs.insert(e.frames_dir);
        }
    }

    std::error_code ec;
    if(!std::filesystem::is_directory(CAMERA_PRINT_VIDEO_OUT_DIR, ec)) {
        return;
    }

    const auto now = std::filesystem::file_time_type::clock::now();
    const auto ttl = std::chrono::seconds(MEDIA_STALE_WORK_TTL_SEC);

    for(const auto& ent : std::filesystem::directory_iterator(CAMERA_PRINT_VIDEO_OUT_DIR, ec)) {
        if(ec) {
            break;
        }
        if(!ent.is_directory(ec) || ec) {
            continue;
        }
        const std::string dir_name = ent.path().filename().string();
        if(dir_name.find(".__work_") == std::string::npos) {
            continue;
        }
        if(keep_frames_dirs.find(dir_name) != keep_frames_dirs.end()) {
            continue;
        }

        std::error_code tm_ec;
        const auto mt = std::filesystem::last_write_time(ent.path(), tm_ec);
        if(tm_ec) {
            continue;
        }
        if((now - mt) < ttl) {
            continue;
        }

        std::error_code rm_ec;
        const uintmax_t removed = std::filesystem::remove_all(ent.path(), rm_ec);
        if(rm_ec) {
            UPC_LOG_MEDIA_SERVICE(Warn,
                "stale work cleanup fail dir=%s err=%s",
                ent.path().c_str(), rm_ec.message().c_str());
        } else {
            UPC_LOG_MEDIA_SERVICE(Info,
                "stale work cleanup removed dir=%s files=%llu",
                ent.path().c_str(),
                static_cast<unsigned long long>(removed));
        }
    }
}

void MediaService::cleanup_work_dirs_before_print_start(
    const middleware::PrintVideoSessionPayload& info) const
{
    std::vector<upc::PrintHistoryEntry> entries;
    (void)upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries);

    std::unordered_set<std::string> keep_frames_dirs;
    for(const auto& e : entries) {
        if((!e.committed || e.video_pending) && !e.frames_dir.empty()) {
            keep_frames_dirs.insert(e.frames_dir);
        }
    }

    keep_frames_dirs.insert(
        build_work_dir_name_for_session(info.job_name, info.session_id));

    std::error_code ec;
    if(!std::filesystem::is_directory(CAMERA_PRINT_VIDEO_OUT_DIR, ec)) {
        return;
    }

    for(const auto& ent : std::filesystem::directory_iterator(CAMERA_PRINT_VIDEO_OUT_DIR, ec)) {
        if(ec) {
            break;
        }
        if(!ent.is_directory(ec) || ec) {
            continue;
        }
        const std::string dir_name = ent.path().filename().string();
        if(dir_name.find(".__work_pv") == std::string::npos) {
            continue;
        }
        if(keep_frames_dirs.find(dir_name) != keep_frames_dirs.end()) {
            continue;
        }

        std::error_code rm_ec;
        const uintmax_t removed = std::filesystem::remove_all(ent.path(), rm_ec);
        if(rm_ec) {
            UPC_LOG_MEDIA_SERVICE(Warn,
                "pre-print work cleanup fail dir=%s err=%s",
                ent.path().c_str(), rm_ec.message().c_str());
        } else {
            UPC_LOG_MEDIA_SERVICE(Info,
                "pre-print work cleanup removed dir=%s files=%llu",
                ent.path().c_str(),
                static_cast<unsigned long long>(removed));
        }
    }
}

} // namespace upc::media
