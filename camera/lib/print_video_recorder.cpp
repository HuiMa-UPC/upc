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

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <vector>

namespace upc::camera {

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
    close_shm_consumer();
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
    open_shm_consumer();
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
            finalize_session_h264(false);
            break;
        case PrintVideoJobKind::AbortCleanup:
            finalize_session_h264(true);
            break;
        }
    }
}

bool PrintVideoRecorder::open_shm_consumer()
{
    if(consumer_open_) {
        return true;
    }
    if(consumer_.open(CAMERA_SHM_NAME_DEFAULT) != upc_ipc::IpcShmError::Ok) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video SHM consumer open failed (preview not running?)");
        return false;
    }
    consumer_open_ = true;
    return true;
}

void PrintVideoRecorder::close_shm_consumer()
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
    if(!open_shm_consumer()) {
        return false;
    }
    std::optional<upc_ipc::IpcShmFrameView> frame = consumer_.acquire_latest_frame();
    if(!frame || !frame->y_plane || frame->width != stream_width_ ||
       frame->height != stream_height_) {
        if(frame) {
            consumer_.release_slot(frame->slot_index);
        }
        return false;
    }

    const size_t need =
        static_cast<size_t>(stream_width_) * stream_height_ * 3u / 2u;
    out.resize(need);
    std::memcpy(out.data(), frame->y_plane, need);
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

void PrintVideoRecorder::finalize_session_h264(bool aborted)
{
    close_shm_consumer();
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
    const std::string out_path =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + job_name_ +
        CAMERA_PRINT_VIDEO_OUT_EXT;

    /* 同名覆盖：rename 在 Linux 上会替换已存在文件；copy 用 wb 截断写 */
    (void)std::remove(out_path.c_str());

    bool saved = false;
    if(std::rename(h264_path_.c_str(), out_path.c_str()) == 0) {
        saved = true;
    } else if(copy_file(h264_path_.c_str(), out_path.c_str())) {
        saved = true;
    }

    if(saved) {
        UPC_LOG_CAMERA_SERVICE(Info,
            "print_video saved -> %s (MPP H.264%s)",
            out_path.c_str(), aborted ? ", aborted" : "");
    } else {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video save failed dst=%s src=%s errno=%d",
            out_path.c_str(), h264_path_.c_str(), errno);
    }

    remove_session_dir();
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

bool PrintVideoRecorder::copy_file(const char* src, const char* dst)
{
    if(!src || !dst) {
        return false;
    }
    FILE* in = std::fopen(src, "rb");
    if(!in) {
        return false;
    }
    FILE* out = std::fopen(dst, "wb");
    if(!out) {
        std::fclose(in);
        return false;
    }

    std::array<uint8_t, 65536> buf{};
    bool ok = true;
    for(;;) {
        const size_t n = std::fread(buf.data(), 1, buf.size(), in);
        if(n == 0) {
            break;
        }
        if(std::fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            break;
        }
    }

    if(std::fclose(out) != 0) {
        ok = false;
    }
    std::fclose(in);
    return ok;
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
