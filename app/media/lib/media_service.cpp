#include "media_service.hpp"

#include "nv12_copy.hpp"
#include "config/cal_config.hpp"
#include "device_config.hpp"
#include "media_paths.hpp"
#include "print_history_manifest.hpp"
#include "rs_log.hpp"
#include "timelapse_util.hpp"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <algorithm>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <filesystem>

namespace upc::media {

namespace {

constexpr uint64_t kStartRetryMs = 1000;
constexpr int64_t kPendingReclaimTimeoutSec = 30 * 60;

/** 初始化当前目录布局（不再兼容旧路径迁移）。 */
void ensure_media_layout_dirs()
{
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::create_directories(UPC_MEDIA_ROOT, ec);
    ec.clear();
    fs::create_directories(UPC_VIDEO_DIR, ec);
    ec.clear();
    fs::create_directories(UPC_PHOTO_DIR, ec);
}

bool path_is_legacy_work_dir(const std::string& dir_name)
{
    return dir_name.find(".__work_") != std::string::npos;
}

/** 本地 RTSP 监听端口：`UPC_LOCAL_RTSP_PORT` 覆盖，非法值回落默认。 */
uint16_t local_rtsp_port()
{
    const char* env = std::getenv("UPC_LOCAL_RTSP_PORT");
    if(env && env[0]) {
        const long v = std::strtol(env, nullptr, 10);
        if(v > 0 && v <= 65535) {
            return static_cast<uint16_t>(v);
        }
    }
    return UPC_LOCAL_RTSP_PORT_DEFAULT;
}

} // namespace

MediaService::~MediaService()
{
    shutdown();
}

bool MediaService::init()
{
    ensure_media_layout_dirs();
    recover_pending_and_reset_tmp();
    subscribe_messages();
    print_video_.set_sender_module_id(middleware::ModuleId::APP_MEDIA);
    print_video_.set_nv12_snapshot_provider([this](std::vector<uint8_t>& out) {
        return copy_latest_nv12_snapshot(out);
    });
    print_video_.set_print_thumb_provider([this](const char* out_path) {
        return print_h264_.create_final_thumb(out_path);
    });
    /* 层拍与 H264 都在写盘热路径上按帧/按层调用，必须走节流版：
     * 全量核算要遍历整个录像目录，逐帧跑会随层数越来越慢。 */
    print_video_.set_storage_budget_guard([this]() {
        return ensure_video_storage_budget_throttled();
    });
    print_h264_.set_storage_budget_guard([this]() {
        return ensure_video_storage_budget_throttled();
    });
    print_h264_.set_finalize_done_callback([this] {
        on_print_h264_finalize_done();
    });
    print_h264_.set_storage_exhausted_callback([this] {
        on_print_h264_storage_exhausted();
    });
    /* 云端推流与本地 RTSP Server 共用同一路 H.264 编码。 */
    live_rtsp_.set_encoder(&h264_encoder_);
    local_rtsp_.set_encoder(&h264_encoder_);
    /* live555 事件循环随本进程常驻；调试开关只控制 session 挂载与编码器 sink。 */
    (void)local_rtsp_.init(local_rtsp_port());

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
    middleware::RtspPushStopReqPayload stop_req{};
    std::snprintf(stop_req.req_id, sizeof(stop_req.req_id), "shutdown");
    std::snprintf(stop_req.stream_type, sizeof(stop_req.stream_type), "live");
    (void)live_rtsp_.stop(stop_req);
    local_rtsp_.shutdown();

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
        middleware::MsgType::APP_Media_Camera_Stream_Stop_Rsp,
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
    subs_.push_back(bus.subscribe(
        middleware::MsgType::MQTT_Media_Rtsp_Start_Req,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::MQTT_Media_Rtsp_Stop_Req,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::UI_Debug_Local_Rtsp_Set_Ntf,
        middleware::ModuleId::APP_MEDIA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::UI_Debug_Local_Rtsp_Get_Ntf,
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
    case middleware::MsgType::MQTT_Media_Rtsp_Start_Req:
        handle_rtsp_start_req(msg);
        break;
    case middleware::MsgType::MQTT_Media_Rtsp_Stop_Req:
        handle_rtsp_stop_req(msg);
        break;
    case middleware::MsgType::UI_Debug_Local_Rtsp_Set_Ntf:
        handle_local_rtsp_set(msg);
        break;
    case middleware::MsgType::UI_Debug_Local_Rtsp_Get_Ntf:
        handle_local_rtsp_get(msg);
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

    print_video_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
    print_h264_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
    clean_video_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
    h264_encoder_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);

    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = true;
        start_req_sent_ = false;
    }

    stop_worker_.store(false);
    worker_ = std::thread(&MediaService::worker_thread_fn, this);

    /* 几何已知、worker 已起，此时才具备开本地 RTSP 的条件。 */
    apply_local_rtsp_desired();
}

/**
 * 按期望状态拉起本地 RTSP（相机流就绪后）。
 * 已在运行则跳过：相机看门狗重启会再次走到这里，重开会白白断掉在连的客户端。
 */
void MediaService::apply_local_rtsp_desired()
{
    if(!local_rtsp_desired_ || local_rtsp_.active()) {
        return;
    }
    if(!local_rtsp_.start()) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn, "local_rtsp auto start failed");
    }
    publish_local_rtsp_state();
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

        /*
         * 持槽期间只做一次紧凑拷贝，随后立刻归还槽位。
         * 下面三路各自还要把帧拷进自己的队列，若留在槽内做，持槽时间会翻几倍，
         * 直接占满 Camera 的在飞窗口并反压采集线程。
         */
        const uint32_t width  = frame->width();
        const uint32_t height = frame->height();
        const uint32_t stride = frame->stride_y();
        bool staged = false;
        if(frame->buffer_idx < dma_fds_.size() &&
           map_dma_views_if_needed() &&
           width > 0 && height > 0 && stride > 0 &&
           frame->buffer_idx < dma_maps_.size()) {
            uint8_t* src = dma_maps_[frame->buffer_idx];
            if(src) {
                staging_nv12_.resize(
                    static_cast<size_t>(width) * height * 3u / 2u);
                upc::copy_nv12_tight(staging_nv12_.data(), src,
                                src + static_cast<size_t>(stride) * height,
                                stride, width, height);
                staged = true;
            }
        }
        consumer_.release_slot(frame->slot_index);

        if(!staged) {
            continue;
        }

        const uint8_t* src_y = staging_nv12_.data();
        const uint8_t* src_uv = src_y + static_cast<size_t>(width) * height;
        /* 仅活跃消费者进 on_frame：未录像/未直播时跳过各自全帧 memcpy。 */
        if(clean_video_active_) {
            clean_video_.on_frame_nv12(src_y, src_uv, width);
        }
        if(print_h264_.session_active()) {
            print_h264_.on_frame_nv12(src_y, src_uv, width);
        }
        if(h264_encoder_.active()) {
            h264_encoder_.on_frame_nv12(src_y, src_uv, width);
        }
        publish_latest_nv12_snapshot(width, height);
    }
}

/**
 * 把本帧的紧凑 NV12（staging_nv12_）发布为最新快照，供拍照与层拍取用。
 *
 * 与 staging 缓冲互换而不是再拷一份：staging 下一帧会被整体覆写，换回来的
 * 旧快照缓冲容量已经够用，resize 不会重新分配。
 */
void MediaService::publish_latest_nv12_snapshot(uint32_t width, uint32_t height)
{
    const size_t need =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3u / 2u;
    if(need == 0 || staging_nv12_.size() < need) {
        return;
    }
    std::lock_guard<std::mutex> lock(latest_nv12_mtx_);
    staging_nv12_.swap(latest_nv12_snapshot_);
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

    if(mkdir(UPC_PHOTO_DIR, 0755) != 0 && errno != EEXIST) {
        UPC_LOG_MEDIA_SERVICE(Error, "mkdir %s failed: %s",
                              UPC_PHOTO_DIR, std::strerror(errno));
        return false;
    }

    const std::string stamp = format_stamp_local();
    const std::string stem = "photo_" + stamp + "_" +
                             std::to_string(stream_width_) + "x" +
                             std::to_string(stream_height_);
    const std::string out_png_path =
        std::string(UPC_PHOTO_DIR) + "/" + stem + ".png";
    const std::string out_rthm_path =
        std::string(UPC_PHOTO_DIR) + "/" + stem + CAMERA_PRINT_VIDEO_THUMB_EXT;
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

    /* 与延时/打印录像一致：同帧 NV12→RTHM 缩略图；失败不影响拍照成功 */
    (void)std::remove(out_rthm_path.c_str());
    if(!timelapse_util::create_raw_thumb_from_nv12(
           nv12, stream_width_, stream_height_, out_rthm_path.c_str())) {
        UPC_LOG_MEDIA_SERVICE(Warn, "snapshot thumb failed path=%s",
                              out_rthm_path.c_str());
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
        /* 开打前先按配额腾挪，再收尾上一单残留并清 tmp */
        (void)ensure_video_storage_budget();
        recover_pending_and_reset_tmp();
        apply_storage_watermark(info);
        {
            std::lock_guard<std::mutex> lock(finalize_mtx_);
            latest_print_session_closed_ = false;
            h264_finalize_pending_ = (info.h264_continuous_on != 0);
        }
        print_video_.set_stream_geometry(stream_width_, stream_height_, stream_fps_);
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
            finalize_frames_dir = take_ready_finalize_locked();
        }
        if(!finalize_frames_dir.empty()) {
            finalize_print_video_by_stage(finalize_frames_dir);
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
        bool can_cleanup = false;
        {
            std::lock_guard<std::mutex> lock(finalize_mtx_);
            can_cleanup = latest_print_session_closed_;
            if(can_cleanup) {
                latest_print_session_closed_ = false;
            }
        }
        if(can_cleanup) {
            const std::string tmp_h264_mp4 =
                std::string(UPC_VIDEO_TMP) + "/print_h264.mp4";
            std::error_code ec;
            const bool mp4_exists = std::filesystem::exists(tmp_h264_mp4, ec) && !ec;
            const uintmax_t mp4_size =
                mp4_exists ? std::filesystem::file_size(tmp_h264_mp4, ec) : 0u;
            const bool mp4_non_empty = mp4_exists && !ec && mp4_size > 0u;
            if(mp4_non_empty) {
                UPC_LOG_MEDIA_SERVICE(
                    Info,
                    "print_video finalize empty req but mp4 exists, promote tmp flow size=%llu",
                    static_cast<unsigned long long>(mp4_size));
                if(!commit_history_with_space(UPC_VIDEO_TMP_NAME)) {
                    UPC_LOG_MEDIA_SERVICE(
                        Warn, "print_video commit tmp still failing after eviction");
                }
                std::string finalize_frames_dir;
                {
                    std::lock_guard<std::mutex> lock(finalize_mtx_);
                    finalize_pending_ = true;
                    pending_finalize_frames_dir_ = UPC_VIDEO_TMP_NAME;
                    finalize_frames_dir = take_ready_finalize_locked();
                }
                if(!finalize_frames_dir.empty()) {
                    finalize_print_video_by_stage(finalize_frames_dir);
                }
                return;
            }
            print_h264_.enqueue_cleanup_tmp();
        } else {
            UPC_LOG_MEDIA_SERVICE(
                Warn, "print_h264 cleanup skipped: stale or empty finalize request");
        }
        return;
    }

    if(!commit_history_with_space(req.frames_dir)) {
        UPC_LOG_MEDIA_SERVICE(
            Warn, "print_video commit still failing after eviction frames_dir=%s",
            req.frames_dir);
    }

    std::string finalize_frames_dir;
    {
        std::lock_guard<std::mutex> lock(finalize_mtx_);
        finalize_pending_ = true;
        pending_finalize_frames_dir_ = req.frames_dir;
        finalize_frames_dir = take_ready_finalize_locked();
    }
    if(!finalize_frames_dir.empty()) {
        finalize_print_video_by_stage(finalize_frames_dir);
    }
}

std::string MediaService::take_ready_finalize_locked()
{
    if(!finalize_pending_ || pending_finalize_frames_dir_.empty() ||
       clean_video_active_) {
        return {};
    }
    std::string frames_dir = std::move(pending_finalize_frames_dir_);
    finalize_pending_ = false;
    return frames_dir;
}

void MediaService::finalize_print_video_by_stage(const std::string& frames_dir)
{
    if(frames_dir.empty()) {
        return;
    }
    /* 固定先释放 Timelapse（清 video_pending），再决定是否等待 H264 第二阶段。 */
    (void)ensure_video_storage_budget();
    print_video_.finalize_after_exit(frames_dir.c_str(), false);

    {
        std::lock_guard<std::mutex> lock(finalize_mtx_);
        if(h264_finalize_pending_) {
            finalize_pending_ = true;
            pending_finalize_frames_dir_ = frames_dir;
            return;
        }
    }
    finalize_print_video_stage2(frames_dir);
}

void MediaService::finalize_print_video_stage2(const std::string& frames_dir)
{
    if(frames_dir.empty()) {
        return;
    }
    (void)ensure_video_storage_budget();
    print_video_.finalize_after_exit(frames_dir.c_str(), true);
}

void MediaService::on_print_h264_finalize_done()
{
    std::string finalize_frames_dir;
    {
        std::lock_guard<std::mutex> lock(finalize_mtx_);
        h264_finalize_pending_ = false;
        finalize_frames_dir = take_ready_finalize_locked();
    }
    if(!finalize_frames_dir.empty()) {
        finalize_print_video_stage2(finalize_frames_dir);
    }
}

void MediaService::on_print_h264_storage_exhausted()
{
    print_video_.disable_h264_expectation();
}

void MediaService::handle_storage_usage_req(const middleware::Message& msg) const
{
    middleware::StorageUsageReqPayload req{};
    if(msg.payload.type == middleware::MsgPayload::Type::Inline &&
       msg.payload.length >= sizeof(req)) {
        (void)std::memcpy(&req, msg.payload.data.bytes.data(), sizeof(req));
    }

    middleware::StorageUsagePayload rsp{};
    rsp.volume = req.volume;

    /* 现只认内置 userdata；SD / U 盘回 present=0，设置页画「未插入」。 */
    if(req.volume != static_cast<uint8_t>(middleware::StorageVolume::Internal)) {
        rsp.present = 0;
        middleware::Message ntf(
            middleware::MsgType::APP_Storage_Usage_Rsp,
            middleware::ModuleId::APP_MEDIA,
            msg.sender_id);
        ntf.payload = middleware::MsgPayload::from_bytes(
            reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
        middleware::MsgPort::instance().publish_async(ntf);
        return;
    }

    struct statvfs vfs {};
    if(::statvfs(UPC_MEDIA_ROOT, &vfs) == 0) {
        const uint64_t block_size = static_cast<uint64_t>(vfs.f_frsize);
        const uint64_t total = static_cast<uint64_t>(vfs.f_blocks) * block_size;
        const uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * block_size;
        rsp.total_bytes = total;
        rsp.used_bytes = (total > free_bytes) ? (total - free_bytes) : 0u;
        rsp.present = 1;
    } else {
        UPC_LOG_MEDIA_SERVICE_TO(StorageUsage, Warn, "storage statvfs failed path=%s err=%d",
                              UPC_MEDIA_ROOT, errno);
        rsp.total_bytes = 0;
        rsp.used_bytes = 0;
        rsp.present = 0;
    }

    middleware::Message ntf(
        middleware::MsgType::APP_Storage_Usage_Rsp,
        middleware::ModuleId::APP_MEDIA,
        msg.sender_id);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    middleware::MsgPort::instance().publish_async(ntf);
}

void MediaService::send_rtsp_rsp(middleware::MsgType type,
                                 const middleware::RtspPushRspPayload& rsp) const
{
    middleware::Message msg(
        type,
        middleware::ModuleId::APP_MEDIA,
        middleware::ModuleId::APP_MQTT);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    if(!middleware::MsgPort::instance().publish_async(msg)) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn, "live_rtsp rsp publish fail type=%u",
                              static_cast<unsigned>(type));
    }
}

void MediaService::handle_rtsp_start_req(const middleware::Message& msg)
{
    middleware::RtspPushStartReqPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp,
            Error,
            "live_rtsp start: bad payload type=%u len=%u need=%zu",
            static_cast<unsigned>(msg.payload.type),
            static_cast<unsigned>(msg.payload.length),
            sizeof(req));
        middleware::RtspPushRspPayload rsp{};
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "encoder_error");
        std::snprintf(rsp.stream_type, sizeof(rsp.stream_type), "live");
        send_rtsp_rsp(middleware::MsgType::APP_Mqtt_Rtsp_Start_Rsp, rsp);
        return;
    }
    req.req_id[sizeof(req.req_id) - 1] = '\0';
    req.push_url[sizeof(req.push_url) - 1] = '\0';
    req.stream_type[sizeof(req.stream_type) - 1] = '\0';

    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if(stream_up_) {
            h264_encoder_.set_stream_geometry(
                stream_width_, stream_height_, stream_fps_);
        }
    }

    const auto rsp = live_rtsp_.start(req);
    send_rtsp_rsp(middleware::MsgType::APP_Mqtt_Rtsp_Start_Rsp, rsp);
}

void MediaService::handle_rtsp_stop_req(const middleware::Message& msg)
{
    middleware::RtspPushStopReqPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        middleware::RtspPushRspPayload rsp{};
        rsp.result = static_cast<uint8_t>(middleware::RtspPushResult::Error);
        std::snprintf(rsp.reason, sizeof(rsp.reason), "encoder_error");
        std::snprintf(rsp.stream_type, sizeof(rsp.stream_type), "live");
        send_rtsp_rsp(middleware::MsgType::APP_Mqtt_Rtsp_Stop_Rsp, rsp);
        return;
    }
    req.req_id[sizeof(req.req_id) - 1] = '\0';
    req.stream_type[sizeof(req.stream_type) - 1] = '\0';
    const auto rsp = live_rtsp_.stop(req);
    send_rtsp_rsp(middleware::MsgType::APP_Mqtt_Rtsp_Stop_Rsp, rsp);
}

/**
 * 调试页开关：开/停本地局域网 RTSP Server。
 * 无论成败都回报实际状态：相机流未就绪时 start() 失败，UI 据此把开关弹回。
 */
void MediaService::handle_local_rtsp_set(const middleware::Message& msg)
{
    middleware::DebugOnOffPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn, "local_rtsp set: bad payload len=%u need=%zu",
                              static_cast<unsigned>(msg.payload.length), sizeof(req));
        publish_local_rtsp_state();
        return;
    }

    /* 记住用户意图：相机流中断再恢复时据此重建，而不是一律回到上电默认值。 */
    local_rtsp_desired_ = (req.on != 0);

    if(req.on != 0) {
        {
            std::lock_guard<std::mutex> lock(state_mtx_);
            if(!stream_up_) {
                UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn, "local_rtsp on rejected: camera stream down");
                publish_local_rtsp_state();
                return;
            }
            h264_encoder_.set_stream_geometry(
                stream_width_, stream_height_, stream_fps_);
        }
        if(!local_rtsp_.start()) {
            UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn, "local_rtsp on failed");
        }
    } else {
        local_rtsp_.stop();
    }
    publish_local_rtsp_state();
}

/** 调试页进入时的状态查询。 */
void MediaService::handle_local_rtsp_get(const middleware::Message& /*msg*/)
{
    publish_local_rtsp_state();
}

/** 向 UI 回报本地 RTSP Server 实际开关状态。 */
void MediaService::publish_local_rtsp_state() const
{
    middleware::DebugOnOffPayload state{};
    state.on = local_rtsp_.active() ? 1u : 0u;

    middleware::Message ntf(
        middleware::MsgType::APP_Media_Local_Rtsp_State_Ntf,
        middleware::ModuleId::APP_MEDIA,
        middleware::ModuleId::UI);
    ntf.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&state), sizeof(state));
    if(!middleware::MsgPort::instance().publish_async(ntf)) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Warn, "local_rtsp state publish fail");
    }
    if(state.on != 0) {
        UPC_LOG_MEDIA_SERVICE_TO(MediaRtsp, Info, "local_rtsp on url=%s", local_rtsp_.url().c_str());
    }
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

bool MediaService::clear_video_tmp_dir() const
{
    std::error_code ec;
    if(!std::filesystem::exists(UPC_VIDEO_TMP, ec)) {
        return false;
    }
    const uintmax_t removed = std::filesystem::remove_all(UPC_VIDEO_TMP, ec);
    if(ec) {
        UPC_LOG_MEDIA_SERVICE_TO(StorageUsage, Warn, "STORRCLM_TMP_FAIL path=%s err=%s",
                              UPC_VIDEO_TMP, ec.message().c_str());
        return false;
    } else if(removed > 0) {
        UPC_LOG_MEDIA_SERVICE_TO(StorageUsage, Info, "STORRCLM_TMP_OK path=%s entries=%llu",
                              UPC_VIDEO_TMP,
                              static_cast<unsigned long long>(removed));
        return true;
    }
    return false;
}

bool MediaService::is_protected_frames_dir(const std::string& frames_dir) const
{
    if(frames_dir.empty()) {
        return true;
    }
    if(frames_dir == UPC_VIDEO_TMP_NAME) {
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(finalize_mtx_);
        if(!pending_finalize_frames_dir_.empty() &&
           frames_dir == pending_finalize_frames_dir_) {
            return true;
        }
    }
    const auto to_frames_dir = [](const std::string& abs_dir) -> std::string {
        if(abs_dir.empty()) {
            return {};
        }
        return std::filesystem::path(abs_dir).filename().string();
    };
    const std::string pv = to_frames_dir(print_video_.current_session_dir());
    if(!pv.empty() && frames_dir == pv) {
        return true;
    }
    const std::string ph = to_frames_dir(print_h264_.current_session_dir());
    if(!ph.empty() && frames_dir == ph) {
        return true;
    }
    return false;
}

bool MediaService::reclaim_orphan_or_stale_pending_for_budget() const
{
    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        return false;
    }
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    bool changed = false;
    bool reclaimed = false;
    std::error_code ec;
    for(auto it = entries.begin(); it != entries.end();) {
        const bool pending = !it->committed || it->video_pending || it->print_video_pending;
        if(!pending || it->frames_dir.empty()) {
            ++it;
            continue;
        }
        if(is_protected_frames_dir(it->frames_dir)) {
            ++it;
            continue;
        }
        const std::string abs =
            std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + it->frames_dir;
        const bool exists = std::filesystem::exists(abs, ec);
        if(!exists && !ec) {
            UPC_LOG_MEDIA_SERVICE_TO(StorageUsage,
                Info, "STORRCLM_ORPHAN_CLEAN frames_dir=%s", it->frames_dir.c_str());
            it = entries.erase(it);
            changed = true;
            reclaimed = true;
            continue;
        }
        if(ec) {
            ec.clear();
            ++it;
            continue;
        }
        const int64_t age = (it->start_unix > 0 && now > it->start_unix)
            ? (now - it->start_unix) : 0;
        if(age < kPendingReclaimTimeoutSec) {
            ++it;
            continue;
        }
        std::error_code rm_ec;
        std::filesystem::remove_all(abs, rm_ec);
        if(rm_ec) {
            ++it;
            continue;
        }
        UPC_LOG_MEDIA_SERVICE_TO(StorageUsage,
            Info,
            "STORRCLM_STALE_PENDING_DEL frames_dir=%s age_sec=%lld",
            it->frames_dir.c_str(), static_cast<long long>(age));
        it = entries.erase(it);
        changed = true;
        reclaimed = true;
    }
    if(!changed) {
        return reclaimed;
    }
    const std::string tmp_path = std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp_path, entries) ||
       std::rename(tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        (void)std::remove(tmp_path.c_str());
        UPC_LOG_MEDIA_SERVICE_TO(StorageUsage, Warn, "STORRCLM_PENDING_SAVE_FAIL");
        return reclaimed;
    }
    return reclaimed;
}

void MediaService::remove_legacy_work_dirs() const
{
    std::error_code ec;
    if(!std::filesystem::is_directory(CAMERA_PRINT_VIDEO_OUT_DIR, ec)) {
        return;
    }
    for(const auto& ent :
        std::filesystem::directory_iterator(CAMERA_PRINT_VIDEO_OUT_DIR, ec)) {
        if(ec) {
            break;
        }
        if(!ent.is_directory(ec) || ec) {
            continue;
        }
        const std::string dir_name = ent.path().filename().string();
        if(!path_is_legacy_work_dir(dir_name)) {
            continue;
        }
        std::error_code rm_ec;
        const uintmax_t removed = std::filesystem::remove_all(ent.path(), rm_ec);
        if(rm_ec) {
            UPC_LOG_MEDIA_SERVICE(Warn,
                "legacy work cleanup fail dir=%s err=%s",
                ent.path().c_str(), rm_ec.message().c_str());
        } else {
            UPC_LOG_MEDIA_SERVICE(Info,
                "legacy work cleanup removed dir=%s files=%llu",
                ent.path().c_str(),
                static_cast<unsigned long long>(removed));
        }
    }
}

void MediaService::drop_orphan_pending_flags() const
{
    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        return;
    }

    bool changed = false;
    std::error_code ec;
    for(auto& e : entries) {
        if(e.frames_dir.empty()) {
            continue;
        }
        const bool pending =
            !e.committed || e.video_pending || e.print_video_pending;
        if(!pending) {
            continue;
        }
        const std::string abs =
            std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + e.frames_dir;
        const bool exists = std::filesystem::exists(abs, ec);
        if(exists) {
            continue;
        }
        /* 目录已不在：清掉锁死的 pending，避免永远占 keep 语义 */
        e.committed = true;
        e.video_pending = false;
        e.print_video_pending = false;
        if(e.frames_dir == UPC_VIDEO_TMP_NAME) {
            e.frames_dir.clear();
        }
        changed = true;
        UPC_LOG_MEDIA_SERVICE(Info,
            "dropped orphan pending job=%s frames_dir=%s",
            e.job_name.c_str(), abs.c_str());
    }

    if(!changed) {
        return;
    }
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
            [](const upc::PrintHistoryEntry& e) {
                return e.frames_dir.empty();
            }),
        entries.end());

    const std::string tmp_path = std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp_path, entries) ||
       std::rename(tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        (void)std::remove(tmp_path.c_str());
        UPC_LOG_MEDIA_SERVICE(Warn, "drop orphan pending save fail");
    }
}

void MediaService::recover_pending_and_reset_tmp()
{
    std::vector<upc::PrintHistoryEntry> entries;
    (void)upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries);

    /* 先尽力 finalize：tmp 或遗留 __work_* 上的 pending 条目 */
    for(const auto& e : entries) {
        if(e.frames_dir.empty()) {
            continue;
        }
        const bool needs =
            !e.committed || e.video_pending || e.print_video_pending;
        if(!needs) {
            continue;
        }
        const bool is_tmp = (e.frames_dir == UPC_VIDEO_TMP_NAME);
        const bool is_work = path_is_legacy_work_dir(e.frames_dir);
        if(!is_tmp && !is_work) {
            /* 正式目录上卡住的 pending：只尝试 finalize，不删正式资产 */
            print_video_.finalize_after_exit(e.frames_dir.c_str());
            continue;
        }
        std::error_code ec;
        const std::string abs =
            std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + e.frames_dir;
        if(std::filesystem::exists(abs, ec)) {
            UPC_LOG_MEDIA_SERVICE(Info,
                "recover pending finalize frames_dir=%s", e.frames_dir.c_str());
            (void)commit_history_with_space(e.frames_dir.c_str());
            (void)ensure_video_storage_budget();
            print_video_.finalize_after_exit(e.frames_dir.c_str());
        }
    }

    drop_orphan_pending_flags();
    clear_video_tmp_dir();
    remove_legacy_work_dirs();
}

int MediaService::query_storage_used_pct() const
{
    struct statvfs vfs {};
    if(::statvfs(UPC_MEDIA_ROOT, &vfs) != 0) {
        return -1;
    }
    const uint64_t total = static_cast<uint64_t>(vfs.f_blocks) *
                           static_cast<uint64_t>(vfs.f_frsize);
    if(total == 0) {
        return -1;
    }
    const uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) *
                                static_cast<uint64_t>(vfs.f_frsize);
    const uint64_t used = (total > free_bytes) ? (total - free_bytes) : 0u;
    return static_cast<int>((used * 100ull) / total);
}

uint64_t MediaService::query_directory_bytes(const char* path)
{
    if(!path || !path[0]) {
        return 0;
    }
    std::error_code ec;
    uint64_t total = 0;
    for(const auto& ent : std::filesystem::recursive_directory_iterator(path, ec)) {
        if(ec) {
            break;
        }
        if(!ent.is_regular_file(ec) || ec) {
            continue;
        }
        total += static_cast<uint64_t>(ent.file_size(ec));
        if(ec) {
            ec.clear();
        }
    }
    return total;
}

bool MediaService::commit_history_with_space(const char* frames_dir)
{
    if(!frames_dir || !frames_dir[0]) {
        return false;
    }
    /* 仅允许合法目录键。误传 gcode 文件名时直接拒绝，避免触发回收误删历史。 */
    if(std::strchr(frames_dir, '.') != nullptr) {
        UPC_LOG_MEDIA_SERVICE(
            Warn, "print_video commit invalid frames_dir=%s (skip eviction)",
            frames_dir);
        return false;
    }
    /* 写 config.json.tmp 只需极少空间；盘将满时先按环形预算腾挪。 */
    (void)ensure_video_storage_budget();
    if(print_video_.commit_after_exit(frames_dir)) {
        return true;
    }
    UPC_LOG_MEDIA_SERVICE(
        Warn, "print_video commit miss frames_dir=%s (skip eviction)",
        frames_dir);
    return false;
}

bool MediaService::evict_oldest_committed_video() const
{
    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        return false;
    }
    std::sort(entries.begin(), entries.end(),
        [](const upc::PrintHistoryEntry& a, const upc::PrintHistoryEntry& b) {
            return a.start_unix < b.start_unix;
        });

    auto victim = std::find_if(entries.begin(), entries.end(),
        [this](const upc::PrintHistoryEntry& e) {
            return e.committed && !e.video_pending && !e.print_video_pending &&
                   !e.frames_dir.empty() && !is_protected_frames_dir(e.frames_dir);
        });
    if(victim == entries.end()) {
        return false;
    }

    const std::string frames_dir = victim->frames_dir;
    const std::string path =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + frames_dir;
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if(ec) {
        UPC_LOG_MEDIA_SERVICE_TO(StorageUsage,
            Warn, "STORRCLM_SESSION_FAIL dir=%s err=%s",
            path.c_str(), ec.message().c_str());
        return false;
    }
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [&frames_dir](const upc::PrintHistoryEntry& e) {
            return e.frames_dir == frames_dir;
        }), entries.end());
    const std::string tmp_path =
        std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp_path, entries) ||
       std::rename(tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        (void)std::remove(tmp_path.c_str());
        UPC_LOG_MEDIA_SERVICE_TO(StorageUsage,
            Warn, "STORRCLM_SESSION_SAVE_FAIL dir=%s", path.c_str());
        return false;
    }
    UPC_LOG_MEDIA_SERVICE_TO(StorageUsage, Info, "STORRCLM_SESSION_DEL dir=%s", path.c_str());
    return true;
}

bool MediaService::ensure_video_storage_budget_throttled() const
{
    const uint64_t now_ms = monotonic_ms();
    {
        std::lock_guard<std::mutex> lock(storage_cache_mtx_);
        if(storage_budget_cached_ok_ && storage_budget_checked_ms_ != 0 &&
           now_ms - storage_budget_checked_ms_ < UPC_MEDIA_STORAGE_BUDGET_CACHE_MS) {
            return true;
        }
    }
    const bool ok = ensure_video_storage_budget();
    {
        std::lock_guard<std::mutex> lock(storage_cache_mtx_);
        storage_budget_cached_ok_ = ok;
        storage_budget_checked_ms_ = now_ms;
    }
    return ok;
}

bool MediaService::ensure_video_storage_budget() const
{
    std::lock_guard<std::mutex> reclaim_lock(storage_mtx_);
    struct statvfs vfs {};
    if(::statvfs(UPC_MEDIA_ROOT, &vfs) != 0 || vfs.f_frsize == 0) {
        return false;
    }
    const uint64_t unit = static_cast<uint64_t>(vfs.f_frsize);
    const uint64_t total_bytes = static_cast<uint64_t>(vfs.f_blocks) * unit;
    const uint64_t min_free =
        total_bytes * static_cast<uint64_t>(UPC_VIDEO_STORAGE_MIN_FREE_PCT) / 100u;
    const uint64_t quota =
        total_bytes * static_cast<uint64_t>(UPC_VIDEO_STORAGE_QUOTA_PCT) / 100u;
    bool reclaimed_any = false;

    if(reclaim_orphan_or_stale_pending_for_budget()) {
        reclaimed_any = true;
        if(::statvfs(UPC_MEDIA_ROOT, &vfs) != 0) {
            return false;
        }
    }
    for(;;) {
        const uint64_t video_bytes = query_directory_bytes(UPC_VIDEO_DIR);
        const uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bavail) * unit;
        if(video_bytes <= quota && free_bytes >= min_free) {
            if(reclaimed_any) {
                UPC_LOG_MEDIA_SERVICE_TO(StorageUsage,
                    Info, "STORRCLM_BUDGET_OK video=%llu quota=%llu free=%llu min_free=%llu",
                    static_cast<unsigned long long>(video_bytes),
                    static_cast<unsigned long long>(quota),
                    static_cast<unsigned long long>(free_bytes),
                    static_cast<unsigned long long>(min_free));
            }
            return true;
        }
        if(!evict_oldest_committed_video()) {
            UPC_LOG_MEDIA_SERVICE_TO(StorageUsage,
                Warn, "STORRCLM_EXHAUST video=%llu quota=%llu free=%llu min_free=%llu",
                static_cast<unsigned long long>(video_bytes),
                static_cast<unsigned long long>(quota),
                static_cast<unsigned long long>(free_bytes),
                static_cast<unsigned long long>(min_free));
            return false;
        }
        reclaimed_any = true;
        if(::statvfs(UPC_MEDIA_ROOT, &vfs) != 0) {
            return false;
        }
    }
}

void MediaService::apply_storage_watermark(middleware::PrintVideoSessionPayload& info)
{
    (void)info;
    if(!ensure_video_storage_budget()) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "video budget unavailable at session start; keep recording and rely on write-path checks");
    }
    print_video_.set_layer_snap_enabled(true);
}

} // namespace upc::media
