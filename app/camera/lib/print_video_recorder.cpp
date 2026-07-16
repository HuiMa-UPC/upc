#include "print_video_recorder.hpp"

#include "device_config.hpp"
#include "rs_log.hpp"

#include <rockchip/mpp_err.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_type.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_rc.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/dma-buf.h>

#include <vector>

namespace upc::camera {

namespace {

void dma_buf_sync_start_read(int fd)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

void dma_buf_sync_end_read(int fd)
{
    if(fd < 0) {
        return;
    }
    struct dma_buf_sync sync {};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

} // namespace

PrintVideoRecorder::~PrintVideoRecorder()
{
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cv_.notify_all();
    }
    if(worker_.joinable()) {
        worker_.join();
    }
    close_encoder();
    close_consumer();
}

void PrintVideoRecorder::set_stream_geometry(uint32_t width, uint32_t height)
{
    if(width > 0) {
        stream_width_ = width;
    }
    if(height > 0) {
        stream_height_ = height;
    }
}

void PrintVideoRecorder::enqueue_job(PrintVideoJob job)
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        jobs_.push(job);
    }
    cv_.notify_one();
}

void PrintVideoRecorder::on_session_start(
    const middleware::PrintVideoSessionPayload& info)
{
    if(!worker_.joinable()) {
        stop_.store(false);
        worker_ = std::thread(&PrintVideoRecorder::worker_fn, this);
    }

    session_id_  = info.session_id;
    for(char& c : session_id_) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if(!std::isalnum(uc) && c != '-' && c != '_' && c != '.') {
            c = '_';
        }
    }
    job_name_    = sanitize_filename(info.job_name);
    if(info.width > 0) {
        stream_width_ = info.width;
    }
    if(info.height > 0) {
        stream_height_ = info.height;
    }
    if(info.layer_fps > 0) {
        layer_fps_ = info.layer_fps;
    }

    close_encoder();
    header_written_ = false;
    mpp_pts_        = 0;

    session_dir_ = std::string(CAMERA_PRINT_VIDEO_TMP_DIR) + "/" + session_id_;
    h264_path_   = session_dir_ + "/layers.h264";

    mkdir_p(CAMERA_PRINT_VIDEO_TMP_DIR);
    remove_session_dir();
    if(mkdir(session_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_video mkdir fail %s: %s",
                               session_dir_.c_str(), std::strerror(errno));
        session_active_ = false;
        return;
    }

    session_active_ = true;
    open_consumer();
    UPC_LOG_CAMERA_SERVICE(Info,
        "print_video session start id=%s job=%s dir=%s %ux%u layer_fps=%u",
        session_id_.c_str(), job_name_.c_str(), session_dir_.c_str(),
        stream_width_, stream_height_, layer_fps_);
}

void PrintVideoRecorder::on_layer_snap(
    const middleware::PrintVideoLayerSnapPayload& snap)
{
    if(!session_active_ || snap.session_id[0] == '\0' ||
       session_id_ != snap.session_id) {
        return;
    }
    PrintVideoJob job{};
    job.kind            = PrintVideoJobKind::LayerSnap;
    job.layer_index     = snap.layer_index;
    job.layer_height_um = snap.layer_height_um;
    enqueue_job(job);
}

void PrintVideoRecorder::on_session_finish(
    const middleware::PrintVideoSessionPayload& info)
{
    if(!session_active_) {
        return;
    }
    if(info.session_id[0] != '\0' && session_id_ != info.session_id) {
        return;
    }
    if(info.job_name[0] != '\0') {
        job_name_ = sanitize_filename(info.job_name);
    }
    session_active_ = false;
    enqueue_job(PrintVideoJob{PrintVideoJobKind::FinishMux, 0, 0});
}

void PrintVideoRecorder::on_session_abort(
    const middleware::PrintVideoSessionPayload& info)
{
    if(session_id_.empty()) {
        return;
    }
    if(info.session_id[0] != '\0' && session_id_ != info.session_id) {
        return;
    }
    if(info.job_name[0] != '\0') {
        job_name_ = sanitize_filename(info.job_name);
    }
    session_active_ = false;
    enqueue_job(PrintVideoJob{PrintVideoJobKind::AbortCleanup, 0, 0});
}

void PrintVideoRecorder::set_dma_fd_lookup(std::function<int(uint32_t buffer_idx)> fn)
{
    fd_lookup_ = std::move(fn);
}

void PrintVideoRecorder::worker_fn()
{
    while(!stop_.load()) {
        PrintVideoJob job{};
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this] {
                return stop_.load() || !jobs_.empty();
            });
            if(stop_.load() && jobs_.empty()) {
                break;
            }
            if(jobs_.empty()) {
                continue;
            }
            job = jobs_.front();
            jobs_.pop();
        }

        switch(job.kind) {
        case PrintVideoJobKind::LayerSnap:
            (void)handle_layer_snap(job);
            break;
        case PrintVideoJobKind::FinishMux:
            finalize_session(false);
            break;
        case PrintVideoJobKind::AbortCleanup:
            finalize_session(true);
            break;
        }
    }
}

bool PrintVideoRecorder::open_consumer()
{
    if(consumer_open_) {
        return true;
    }
    if(consumer_.open(CAMERA_SHM_NAME_DEFAULT) != upc_ipc::IpcShmError::OK) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video SHM consumer open failed (preview not running?)");
        return false;
    }
    consumer_open_ = true;
    return true;
}

void PrintVideoRecorder::close_consumer()
{
    if(consumer_open_) {
        consumer_.close();
        consumer_open_ = false;
    }
}

bool PrintVideoRecorder::ensure_encoder()
{
    if(encoder_open_) {
        return true;
    }

    MppCtx  ctx  = nullptr;
    MppApi* mpi  = nullptr;
    if(mpp_create(&ctx, &mpi) != MPP_OK || !ctx || !mpi) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_video mpp_create failed");
        return false;
    }

    if(mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_video mpp_init ENC AVC failed");
        mpp_destroy(ctx);
        return false;
    }

    MppEncCfg enc_cfg = nullptr;
    if(mpp_enc_cfg_init(&enc_cfg) != MPP_OK || !enc_cfg) {
        mpp_destroy(ctx);
        return false;
    }

    const RK_U32 w = stream_width_;
    const RK_U32 h = stream_height_;
    const RK_U32 fps = layer_fps_ > 0 ? layer_fps_ : CAMERA_PRINT_VIDEO_LAYER_FPS;
    const RK_U32 bps = w * h * fps / 8;

    mpp_enc_cfg_set_s32(enc_cfg, "prep:width", static_cast<RK_S32>(w));
    mpp_enc_cfg_set_s32(enc_cfg, "prep:height", static_cast<RK_S32>(h));
    mpp_enc_cfg_set_s32(enc_cfg, "prep:hor_stride", static_cast<RK_S32>(w));
    mpp_enc_cfg_set_s32(enc_cfg, "prep:ver_stride", static_cast<RK_S32>(h));
    mpp_enc_cfg_set_s32(enc_cfg, "prep:format", MPP_FMT_YUV420SP);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_target", static_cast<RK_S32>(bps));
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_max", static_cast<RK_S32>(bps * 3 / 2));
    mpp_enc_cfg_set_s32(enc_cfg, "rc:bps_min", static_cast<RK_S32>(bps / 2));
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_num", static_cast<RK_S32>(fps));
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_num", static_cast<RK_S32>(fps));
    mpp_enc_cfg_set_s32(enc_cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(enc_cfg, "rc:gop", 1);

    if(mpi->control(ctx, MPP_ENC_SET_CFG, enc_cfg) != MPP_OK) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_video MPP_ENC_SET_CFG failed");
        mpp_enc_cfg_deinit(enc_cfg);
        mpp_destroy(ctx);
        return false;
    }
    mpp_enc_cfg_deinit(enc_cfg);

    MppBufferGroup grp = nullptr;
    if(mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_ION) != MPP_OK ||
       !grp) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_video mpp_buffer_group_get failed");
        mpp_destroy(ctx);
        return false;
    }

    mpp_ctx_     = ctx;
    mpp_api_     = mpi;
    mpp_buf_grp_ = grp;
    encoder_open_ = true;
    mpp_pts_      = 0;
    header_written_ = false;
    return true;
}

void PrintVideoRecorder::close_encoder()
{
    if(!encoder_open_) {
        return;
    }
    if(mpp_ctx_) {
        auto* ctx = static_cast<MppCtx>(mpp_ctx_);
        mpp_destroy(ctx);
    }
    if(mpp_buf_grp_) {
        mpp_buffer_group_put(static_cast<MppBufferGroup>(mpp_buf_grp_));
    }
    mpp_ctx_      = nullptr;
    mpp_api_      = nullptr;
    mpp_buf_grp_  = nullptr;
    encoder_open_ = false;
    /* 勿清 header_written_：finish 需据此判断 h264 是否已有数据 */
}

bool PrintVideoRecorder::drain_encoder_packets(FILE* fp)
{
    if(!encoder_open_ || !fp || !mpp_ctx_ || !mpp_api_) {
        return false;
    }

    auto* mpi = static_cast<MppApi*>(mpp_api_);
    auto* ctx = static_cast<MppCtx>(mpp_ctx_);

    bool wrote = false;
    for(;;) {
        MppPacket packet = nullptr;
        const MPP_RET ret = mpi->encode_get_packet(ctx, &packet);
        if(ret != MPP_OK || !packet) {
            break;
        }

        void*  data = mpp_packet_get_pos(packet);
        size_t len  = mpp_packet_get_length(packet);
        if(data && len > 0) {
            if(std::fwrite(data, 1, len, fp) == len) {
                wrote = true;
            } else {
                UPC_LOG_CAMERA_SERVICE(Error, "print_video h264 write fail");
            }
        }

        const RK_U32 eos = mpp_packet_get_eos(packet);
        mpp_packet_deinit(&packet);
        if(eos) {
            break;
        }
    }
    return wrote;
}

bool PrintVideoRecorder::append_encoder_output_to_h264()
{
    FILE* fp = std::fopen(h264_path_.c_str(), header_written_ ? "ab" : "wb");
    if(!fp) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_video open h264 fail %s",
                               h264_path_.c_str());
        return false;
    }

    const bool wrote = drain_encoder_packets(fp);
    std::fclose(fp);
    if(wrote) {
        header_written_ = true;
    }
    return wrote;
}

bool PrintVideoRecorder::flush_encoder_eos()
{
    if(!encoder_open_ || !mpp_ctx_ || !mpp_api_) {
        return false;
    }

    auto* mpi = static_cast<MppApi*>(mpp_api_);
    auto* ctx = static_cast<MppCtx>(mpp_ctx_);

    MppFrame frame = nullptr;
    if(mpp_frame_init(&frame) != MPP_OK || !frame) {
        return false;
    }
    mpp_frame_set_eos(frame, 1);

    const MPP_RET put_ret = mpi->encode_put_frame(ctx, frame);
    mpp_frame_deinit(&frame);
    if(put_ret != MPP_OK) {
        UPC_LOG_CAMERA_SERVICE(Warn, "print_video MPP EOS put_frame fail");
        return false;
    }

    return append_encoder_output_to_h264();
}

bool PrintVideoRecorder::grab_latest_nv12(std::vector<uint8_t>& out)
{
    if(!open_consumer()) {
        return false;
    }
    std::optional<upc_ipc::IpcDmaFrameView> frame = consumer_.acquire_latest_frame();
    if(!frame || frame->width() != stream_width_ ||
       frame->height() != stream_height_) {
        if(frame) {
            consumer_.release_slot(frame->slot_index);
        }
        return false;
    }

    const size_t y_size = static_cast<size_t>(stream_width_) * stream_height_;
    const size_t uv_size = y_size / 2u;
    const size_t need = y_size + uv_size;
    out.resize(need);

    if(!fd_lookup_) {
        consumer_.release_slot(frame->slot_index);
        return false;
    }

    const int fd = fd_lookup_(frame->buffer_idx);
    if(fd < 0) {
        consumer_.release_slot(frame->slot_index);
        return false;
    }

    const uint32_t src_stride = consumer_.stride_y();
    const size_t src_size = static_cast<size_t>(src_stride) * stream_height_ * 3u / 2u;
    dma_buf_sync_start_read(fd);
    void* va = mmap(nullptr, src_size, PROT_READ, MAP_SHARED, fd, 0);
    if(va == MAP_FAILED) {
        dma_buf_sync_end_read(fd);
        consumer_.release_slot(frame->slot_index);
        return false;
    }

    const uint8_t* src_y  = static_cast<const uint8_t*>(va);
    const uint8_t* src_uv = src_y + static_cast<size_t>(src_stride) * stream_height_;

    if(src_stride == stream_width_) {
        std::memcpy(out.data(), src_y, need);
    } else {
        uint8_t* dst_y  = out.data();
        uint8_t* dst_uv = dst_y + y_size;
        for(uint32_t row = 0; row < stream_height_; ++row) {
            std::memcpy(dst_y + static_cast<size_t>(row) * stream_width_,
                        src_y + static_cast<size_t>(row) * src_stride,
                        stream_width_);
        }
        for(uint32_t row = 0; row < stream_height_ / 2u; ++row) {
            std::memcpy(dst_uv + static_cast<size_t>(row) * stream_width_,
                        src_uv + static_cast<size_t>(row) * src_stride,
                        stream_width_);
        }
    }

    munmap(va, src_size);
    dma_buf_sync_end_read(fd);
    consumer_.release_slot(frame->slot_index);
    return true;
}

bool PrintVideoRecorder::encode_nv12_frame(const uint8_t* nv12, size_t bytes,
                                             uint32_t layer_index)
{
    if(!nv12 || !ensure_encoder()) {
        return false;
    }

    const size_t need =
        static_cast<size_t>(stream_width_) * stream_height_ * 3u / 2u;
    if(bytes < need) {
        return false;
    }

    auto* mpi = static_cast<MppApi*>(mpp_api_);
    auto* ctx = static_cast<MppCtx>(mpp_ctx_);
    auto* grp = static_cast<MppBufferGroup>(mpp_buf_grp_);

    MppBuffer buf = nullptr;
    if(mpp_buffer_get(grp, &buf, need) != MPP_OK || !buf) {
        return false;
    }

    void* ptr = mpp_buffer_get_ptr(buf);
    if(!ptr) {
        mpp_buffer_put(buf);
        return false;
    }
    std::memcpy(ptr, nv12, need);

    MppFrame frame = nullptr;
    if(mpp_frame_init(&frame) != MPP_OK || !frame) {
        mpp_buffer_put(buf);
        return false;
    }

    mpp_frame_set_width(frame, stream_width_);
    mpp_frame_set_height(frame, stream_height_);
    mpp_frame_set_hor_stride(frame, stream_width_);
    mpp_frame_set_ver_stride(frame, stream_height_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, buf);
    mpp_frame_set_pts(frame, mpp_pts_++);

    MPP_RET ret = mpi->encode_put_frame(ctx, frame);
    mpp_frame_deinit(&frame);
    mpp_buffer_put(buf);
    if(ret != MPP_OK) {
        UPC_LOG_CAMERA_SERVICE(Warn, "print_video encode_put_frame fail layer=%u",
                               layer_index);
        return false;
    }

    return append_encoder_output_to_h264();
}

bool PrintVideoRecorder::handle_layer_snap(const PrintVideoJob& job)
{
    std::vector<uint8_t> nv12;
    if(!grab_latest_nv12(nv12)) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video snap miss SHM frame session=%s layer=%u",
            session_id_.c_str(), job.layer_index);
        return false;
    }
    if(!encode_nv12_frame(nv12.data(), nv12.size(), job.layer_index)) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video encode fail session=%s layer=%u",
            session_id_.c_str(), job.layer_index);
        return false;
    }
    UPC_LOG_CAMERA_SERVICE(Info,
        "print_video snap ok session=%s layer=%u height_um=%u bytes=%zu",
        session_id_.c_str(), job.layer_index,
        static_cast<unsigned>(job.layer_height_um), nv12.size());
    return true;
}

int PrintVideoRecorder::run_shell_cmd(const char* cmd, const char* log_tag)
{
    if(!cmd || !cmd[0]) {
        return -1;
    }
    UPC_LOG_CAMERA_SERVICE(Info, "print_video %s: %s",
                           log_tag ? log_tag : "cmd", cmd);
    const int rc = std::system(cmd);
    if(rc != 0) {
        UPC_LOG_CAMERA_SERVICE(Error,
            "print_video %s failed rc=%d",
            log_tag ? log_tag : "cmd", rc);
    }
    return rc;
}

bool PrintVideoRecorder::remux_h264_to_mp4(const char* h264_path,
                                           const char* mp4_path) const
{
    if(!h264_path || !h264_path[0] || !mp4_path || !mp4_path[0]) {
        return false;
    }

    const uint32_t fps =
        layer_fps_ > 0 ? layer_fps_ : CAMERA_PRINT_VIDEO_LAYER_FPS;

    /* 裸流无容器时间轴：须 -framerate；分辨率由 Annex-B 内 SPS/PPS 解析 */
    char cmd[1024];
    std::snprintf(
        cmd, sizeof(cmd),
        "%s -y -loglevel error "
        "-f h264 -framerate %u -i '%s' "
        "-c copy -movflags +faststart '%s'",
        CAMERA_FFMPEG_BIN, fps, h264_path, mp4_path);
    return run_shell_cmd(cmd, "ffmpeg remux") == 0;
}

bool PrintVideoRecorder::extract_last_frame_thumb(const char* mp4_path,
                                                  const char* jpg_path) const
{
    if(!mp4_path || !mp4_path[0] || !jpg_path || !jpg_path[0]) {
        return false;
    }

    /* -sseof -1：从文件末尾往前，取最后一帧作为缩略图 */
    char cmd[1024];
    std::snprintf(
        cmd, sizeof(cmd),
        "%s -y -loglevel error -sseof -1 -i '%s' -frames:v 1 -q:v 5 '%s'",
        CAMERA_FFMPEG_BIN, mp4_path, jpg_path);
    return run_shell_cmd(cmd, "ffmpeg thumb") == 0;
}

void PrintVideoRecorder::finalize_session(bool aborted)
{
    close_consumer();
    if(encoder_open_) {
        (void)flush_encoder_eos();
    }

    const bool had_h264 = header_written_;
    close_encoder();

    if(!had_h264) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video %s with no frames session=%s",
            aborted ? "abort" : "finish", session_id_.c_str());
        remove_session_dir();
        session_id_.clear();
        session_dir_.clear();
        h264_path_.clear();
        header_written_ = false;
        return;
    }

    mkdir_p(CAMERA_PRINT_VIDEO_OUT_DIR);
    const std::string mp4_path =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + job_name_ +
        CAMERA_PRINT_VIDEO_OUT_EXT;
    const std::string jpg_path =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + job_name_ +
        CAMERA_PRINT_VIDEO_THUMB_EXT;

    (void)std::remove(mp4_path.c_str());
    (void)std::remove(jpg_path.c_str());

    if(!remux_h264_to_mp4(h264_path_.c_str(), mp4_path.c_str())) {
        UPC_LOG_CAMERA_SERVICE(Error,
            "print_video remux fail keep h264=%s dst=%s%s",
            h264_path_.c_str(), mp4_path.c_str(),
            aborted ? " (aborted)" : "");
        /* remux 失败：保留 /tmp 下裸流便于排查，不清 session 目录 */
        session_id_.clear();
        /* 故意不清 session_dir_/h264_path_ 的磁盘文件；仅复位状态 */
        session_dir_.clear();
        h264_path_.clear();
        header_written_ = false;
        return;
    }

    if(!extract_last_frame_thumb(mp4_path.c_str(), jpg_path.c_str())) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video thumb fail mp4=%s jpg=%s (mp4 kept)",
            mp4_path.c_str(), jpg_path.c_str());
    }

    /* remux 成功后删除临时裸流并清会话目录 */
    (void)std::remove(h264_path_.c_str());
    remove_session_dir();

    UPC_LOG_CAMERA_SERVICE(Info,
        "print_video saved -> %s (+%s)%s",
        mp4_path.c_str(), jpg_path.c_str(), aborted ? " aborted" : "");

    session_id_.clear();
    session_dir_.clear();
    h264_path_.clear();
    header_written_ = false;
}

bool PrintVideoRecorder::remove_session_dir()
{
    if(session_dir_.empty()) {
        return true;
    }
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd), "rm -rf '%s'", session_dir_.c_str());
    return std::system(cmd) == 0;
}

bool PrintVideoRecorder::mkdir_p(const char* path)
{
    if(!path || !path[0]) {
        return false;
    }
    char tmp[256];
    std::snprintf(tmp, sizeof(tmp), "%s", path);
    const size_t len = std::strlen(tmp);
    if(len == 0) {
        return false;
    }
    for(size_t i = 1; i < len; ++i) {
        if(tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        if(mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            return false;
        }
        tmp[i] = '/';
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

std::string PrintVideoRecorder::sanitize_filename(const char* name)
{
    if(!name || !name[0]) {
        return "print";
    }
    std::string s(name);
    for(char& c : s) {
        if(c == '/' || c == '\\' || c == ' ' || c == ':' || c == ';') {
            c = '_';
        }
    }
    if(s.empty()) {
        s = "print";
    }
    return s;
}

} // namespace upc::camera
