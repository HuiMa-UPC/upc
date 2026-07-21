#include "print_video_recorder.hpp"

#include "device_config.hpp"
#include "gcode_manifest.hpp"
#include "print_history_manifest.hpp"
#include "raw_thumb_format.hpp"
#include "rs_log.hpp"

#include <rockchip/mpp_err.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_type.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_rc.h>

#if defined(UPC_CAMERA_HAS_RGA) && UPC_CAMERA_HAS_RGA
#include <rga/im2d.hpp>
#include <rga/rga.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

namespace upc::camera {

namespace {

#if defined(UPC_CAMERA_HAS_RGA) && UPC_CAMERA_HAS_RGA
constexpr int kThumbSrcFormat = RK_FORMAT_YCbCr_420_SP;
constexpr int kThumbDstFormat = RK_FORMAT_RGB_565;

bool rga_status_ok(IM_STATUS st)
{
    return st == IM_STATUS_SUCCESS || st == IM_STATUS_NOERROR;
}
#endif

uint32_t clamp_thumb_width(uint32_t src_w)
{
    const uint32_t max_w = CAMERA_PRINT_VIDEO_THUMB_MAX_W > 0
        ? static_cast<uint32_t>(CAMERA_PRINT_VIDEO_THUMB_MAX_W)
        : src_w;
    return std::max<uint32_t>(1u, std::min(src_w, max_w));
}

uint32_t thumb_height_from_width(uint32_t src_w, uint32_t src_h, uint32_t dst_w)
{
    if(src_w == 0 || src_h == 0 || dst_w == 0) {
        return 1;
    }
    const uint64_t scaled = static_cast<uint64_t>(src_h) * dst_w;
    return std::max<uint32_t>(1u, static_cast<uint32_t>(scaled / src_w));
}

bool write_raw_thumb(const char* out_path,
                     uint32_t width,
                     uint32_t height,
                     uint32_t stride_bytes,
                     const std::vector<uint8_t>& data)
{
    if(!out_path || !out_path[0] || width == 0 || height == 0 ||
       stride_bytes == 0 || data.empty()) {
        return false;
    }

    upc::RawThumbHeader h{};
    std::memcpy(h.magic, upc::kRawThumbMagic, sizeof(h.magic));
    h.version = upc::kRawThumbVersion;
    h.width = width;
    h.height = height;
    h.stride_bytes = stride_bytes;
    h.color_format = static_cast<uint32_t>(upc::RawThumbColorFormat::RGB565);
    h.data_size = static_cast<uint32_t>(data.size());
    if(!upc::raw_thumb_header_valid(h)) {
        return false;
    }

    FILE* fp = std::fopen(out_path, "wb");
    if(!fp) {
        return false;
    }

    bool ok = std::fwrite(&h, 1, sizeof(h), fp) == sizeof(h) &&
              std::fwrite(data.data(), 1, data.size(), fp) == data.size();
    if(std::fclose(fp) != 0) {
        ok = false;
    }
    return ok;
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
    start_unix_  = static_cast<int64_t>(std::time(nullptr));
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
    UPC_LOG_CAMERA_SERVICE(Info,
        "print_video session start id=%s job=%s dir=%s %ux%u layer_fps=%u start_unix=%lld",
        session_id_.c_str(), job_name_.c_str(), session_dir_.c_str(),
        stream_width_, stream_height_, layer_fps_,
        static_cast<long long>(start_unix_));
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

void PrintVideoRecorder::set_nv12_snapshot_provider(
    std::function<bool(std::vector<uint8_t>&)> fn)
{
    snapshot_provider_ = std::move(fn);
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
    if(!snapshot_provider_) {
        return false;
    }
    return snapshot_provider_(out);
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

bool PrintVideoRecorder::create_raw_thumb_from_nv12(
    const std::vector<uint8_t>& nv12, const char* out_path) const
{
#if defined(UPC_CAMERA_HAS_RGA) && UPC_CAMERA_HAS_RGA
    const uint32_t src_w = stream_width_;
    const uint32_t src_h = stream_height_;
    if(src_w == 0 || src_h == 0 || nv12.empty() || !out_path || !out_path[0]) {
        return false;
    }
    const size_t need = static_cast<size_t>(src_w) * src_h * 3u / 2u;
    if(nv12.size() < need) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "raw thumb nv12 too small bytes=%zu need=%zu",
            nv12.size(), need);
        return false;
    }
    if(src_w > static_cast<uint32_t>(INT_MAX) ||
       src_h > static_cast<uint32_t>(INT_MAX)) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "raw thumb geometry too large w=%u h=%u", src_w, src_h);
        return false;
    }
    const int src_w_i = static_cast<int>(src_w);
    const int src_h_i = static_cast<int>(src_h);

    std::vector<uint8_t> rgb565_full(static_cast<size_t>(src_w) * src_h * 2u);
    rga_buffer_t src_buf = wrapbuffer_virtualaddr(
        const_cast<uint8_t*>(nv12.data()), src_w_i, src_h_i, kThumbSrcFormat, src_w_i, src_h_i);
    rga_buffer_t full_buf = wrapbuffer_virtualaddr(
        rgb565_full.data(), src_w_i, src_h_i, kThumbDstFormat, src_w_i, src_h_i);

    IM_STATUS st = imcvtcolor_t(src_buf, full_buf, kThumbSrcFormat, kThumbDstFormat,
                                IM_COLOR_SPACE_DEFAULT, 1);
    if(!rga_status_ok(st)) {
        UPC_LOG_CAMERA_SERVICE(Warn, "raw thumb rga cvt failed st=%d (%s)",
                               st, imStrError(st));
        return false;
    }

    const uint32_t dst_w = clamp_thumb_width(src_w);
    const uint32_t dst_h = thumb_height_from_width(src_w, src_h, dst_w);
    const uint32_t dst_stride_px = dst_w;
    const uint32_t dst_stride_bytes = dst_stride_px * 2u;
    if(dst_w > static_cast<uint32_t>(INT_MAX) ||
       dst_h > static_cast<uint32_t>(INT_MAX) ||
       dst_stride_px > static_cast<uint32_t>(INT_MAX)) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "raw thumb dst too large w=%u h=%u stride_px=%u",
            dst_w, dst_h, dst_stride_px);
        return false;
    }
    const int dst_w_i = static_cast<int>(dst_w);
    const int dst_h_i = static_cast<int>(dst_h);
    const int dst_stride_px_i = static_cast<int>(dst_stride_px);
    std::vector<uint8_t> rgb565_thumb(
        static_cast<size_t>(dst_stride_bytes) * dst_h);

    rga_buffer_t in_buf = wrapbuffer_virtualaddr(
        rgb565_full.data(), src_w_i, src_h_i, kThumbDstFormat, src_w_i, src_h_i);
    rga_buffer_t out_buf = wrapbuffer_virtualaddr(
        rgb565_thumb.data(), dst_w_i, dst_h_i, kThumbDstFormat, dst_stride_px_i, dst_h_i);
    st = imresize(in_buf, out_buf);
    if(!rga_status_ok(st)) {
        UPC_LOG_CAMERA_SERVICE(Warn, "raw thumb rga resize failed st=%d (%s)",
                               st, imStrError(st));
        return false;
    }

    if(!write_raw_thumb(out_path, dst_w, dst_h, dst_stride_bytes, rgb565_thumb)) {
        UPC_LOG_CAMERA_SERVICE(Warn, "raw thumb write failed path=%s", out_path);
        return false;
    }
    return true;
#else
    (void)nv12;
    (void)out_path;
    return false;
#endif
}

bool PrintVideoRecorder::extract_last_frame_thumb(const char* mp4_path,
                                                  const char* jpg_path) const
{
    if(!mp4_path || !mp4_path[0] || !jpg_path || !jpg_path[0]) {
        return false;
    }

    /* -sseof -1：从文件末尾往前，取最后一帧作为缩略图。
     * -vf scale=W:-1：按最大宽度等比缩小，降低历史页滚动时 JPEG 解码负担。 */
    char cmd[1024];
    std::snprintf(
        cmd, sizeof(cmd),
        "%s -y -loglevel error -sseof -1 -i '%s' -frames:v 1 "
        "-vf scale=%d:-1 -q:v 5 '%s'",
        CAMERA_FFMPEG_BIN, mp4_path, CAMERA_PRINT_VIDEO_THUMB_MAX_W, jpg_path);
    return run_shell_cmd(cmd, "ffmpeg thumb") == 0;
}

void PrintVideoRecorder::finalize_session(bool aborted)
{
    if(encoder_open_) {
        (void)flush_encoder_eos();
    }

    const bool had_h264 = header_written_;
    close_encoder();

    if(!had_h264) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video %s with no frames session=%s (no manifest)",
            aborted ? "abort" : "finish", session_id_.c_str());
        remove_session_dir();
        session_id_.clear();
        session_dir_.clear();
        h264_path_.clear();
        start_unix_ = 0;
        header_written_ = false;
        return;
    }

    mkdir_p(CAMERA_PRINT_VIDEO_OUT_DIR);

    /* 命名：{job_name}_{YYYYMMDD_HHMMSS}.mp4 / thumb（同名打印保留多份） */
    const int64_t stamp_unix =
        start_unix_ > 0 ? start_unix_ : static_cast<int64_t>(std::time(nullptr));
    const std::string stamp = format_stamp_local(stamp_unix);
    const std::string base =
        (job_name_.empty() ? std::string("print") : job_name_) + "_" + stamp;
    const std::string video_name = base + CAMERA_PRINT_VIDEO_OUT_EXT;
    const std::string raw_thumb_name = base + CAMERA_PRINT_VIDEO_THUMB_EXT;
    const std::string jpg_thumb_name = base + CAMERA_PRINT_VIDEO_THUMB_FALLBACK_EXT;
    const std::string mp4_path =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + video_name;
    const std::string raw_thumb_path =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + raw_thumb_name;
    const std::string jpg_thumb_path =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + jpg_thumb_name;

    (void)std::remove(mp4_path.c_str());
    (void)std::remove(raw_thumb_path.c_str());
    (void)std::remove(jpg_thumb_path.c_str());

    if(!remux_h264_to_mp4(h264_path_.c_str(), mp4_path.c_str())) {
        UPC_LOG_CAMERA_SERVICE(Error,
            "print_video remux fail keep h264=%s dst=%s%s",
            h264_path_.c_str(), mp4_path.c_str(),
            aborted ? " (aborted)" : "");
        /* remux 失败：保留 /tmp 下裸流便于排查，不清 session 目录、不写 manifest */
        session_id_.clear();
        session_dir_.clear();
        h264_path_.clear();
        start_unix_ = 0;
        header_written_ = false;
        return;
    }

    bool thumb_ok = false;
    std::string thumb_name = raw_thumb_name;
    std::string thumb_path = raw_thumb_path;

    std::vector<uint8_t> nv12;
    if(grab_latest_nv12(nv12) && create_raw_thumb_from_nv12(nv12, raw_thumb_path.c_str())) {
        thumb_ok = true;
    } else if(extract_last_frame_thumb(mp4_path.c_str(), jpg_thumb_path.c_str())) {
        thumb_ok = true;
        thumb_name = jpg_thumb_name;
        thumb_path = jpg_thumb_path;
    } else {
        thumb_name.clear();
        thumb_path.clear();
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video thumb fail raw=%s jpg=%s (mp4 kept)",
            raw_thumb_path.c_str(), jpg_thumb_path.c_str());
    }

    upsert_print_history(aborted, video_name, thumb_name);

    /* remux 成功后删除临时裸流并清会话目录 */
    (void)std::remove(h264_path_.c_str());
    remove_session_dir();

    UPC_LOG_CAMERA_SERVICE(Info,
        "print_video saved -> %s (+%s)%s",
        mp4_path.c_str(),
        thumb_ok ? thumb_path.c_str() : "(none)",
        aborted ? " aborted" : "");

    session_id_.clear();
    session_dir_.clear();
    h264_path_.clear();
    start_unix_ = 0;
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

std::string PrintVideoRecorder::format_stamp_local(int64_t unix_sec)
{
    std::time_t t = static_cast<std::time_t>(unix_sec);
    if(t <= 0) {
        t = std::time(nullptr);
    }
    std::tm tm_buf{};
    if(localtime_r(&t, &tm_buf) == nullptr) {
        return "19700101_000000";
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buf;
}

void PrintVideoRecorder::upsert_print_history(
    bool aborted, const std::string& video_name,
    const std::string& thumb_name) const
{
    if(video_name.empty()) {
        return;
    }

    const int64_t start =
        start_unix_ > 0 ? start_unix_ : static_cast<int64_t>(std::time(nullptr));
    const int64_t end = static_cast<int64_t>(std::time(nullptr));
    uint32_t duration = 0;
    if(end > start) {
        duration = static_cast<uint32_t>(end - start);
    }

    upc::PrintHistoryEntry entry;
    entry.job_name     = job_name_.empty() ? "print" : job_name_;
    entry.start_unix   = start;
    entry.duration_sec = duration;
    entry.material_g   = upc::print_history_lookup_material_g(
        UPC_LOCAL_GCODE_CONFIG, entry.job_name);
    entry.status = aborted ? upc::PrintHistoryStatus::Terminated
                           : upc::PrintHistoryStatus::Complete;
    entry.video  = video_name;
    entry.thumb  = thumb_name;

    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_history load fail path=%s (rewrite)",
            UPC_PRINT_VIDEO_MANIFEST);
        entries.clear();
    }
    upc::print_history_manifest_upsert(entries, entry);

    /* 写临时文件再 rename，避免 UI 读到半截 JSON */
    const std::string tmp_path =
        std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp_path, entries)) {
        UPC_LOG_CAMERA_SERVICE(Error,
            "print_history save fail tmp=%s", tmp_path.c_str());
        (void)std::remove(tmp_path.c_str());
        return;
    }
    if(std::rename(tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        UPC_LOG_CAMERA_SERVICE(Error,
            "print_history rename fail %s -> %s: %s",
            tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST, std::strerror(errno));
        (void)std::remove(tmp_path.c_str());
        return;
    }

    UPC_LOG_CAMERA_SERVICE(Info,
        "print_history upsert job=%s status=%s video=%s duration=%u material_g=%.2f",
        entry.job_name.c_str(), upc::print_history_status_str(entry.status),
        entry.video.c_str(), entry.duration_sec,
        static_cast<double>(entry.material_g));
}

} // namespace upc::camera
