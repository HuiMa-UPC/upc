#include "print_video_recorder.hpp"

#include "device_config.hpp"
#include "gcode_manifest.hpp"
#include "print_history_manifest.hpp"
#include "raw_thumb_format.hpp"
#include "rs_log.hpp"

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
#include <filesystem>
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

    session_id_ = info.session_id;
    for(char& c : session_id_) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if(!std::isalnum(uc) && c != '-' && c != '_' && c != '.') {
            c = '_';
        }
    }

    job_name_   = sanitize_filename(info.job_name);
    start_unix_ = static_cast<int64_t>(std::time(nullptr));
    if(info.width > 0) {
        stream_width_ = info.width;
    }
    if(info.height > 0) {
        stream_height_ = info.height;
    }
    if(info.layer_fps > 0) {
        layer_fps_ = info.layer_fps;
    }

    frame_count_ = 0;
    last_layer_nv12_.clear();
    const int64_t stamp_unix = start_unix_ > 0 ? start_unix_
                                                : static_cast<int64_t>(std::time(nullptr));
    session_name_ = (job_name_.empty() ? std::string("print") : job_name_) + "_" +
        format_stamp_local(stamp_unix);
    session_dir_ = std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + session_name_;

    mkdir_p(CAMERA_PRINT_VIDEO_OUT_DIR);
    remove_session_dir();
    if(mkdir(session_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_video mkdir fail %s: %s",
                               session_dir_.c_str(), std::strerror(errno));
        session_active_ = false;
        return;
    }

    session_active_ = true;
    UPC_LOG_CAMERA_SERVICE(
        Info,
        "print_video session start id=%s job=%s dir=%s %ux%u layer_fps=%u start_unix=%lld",
        session_id_.c_str(),
        job_name_.c_str(),
        session_dir_.c_str(),
        stream_width_,
        stream_height_,
        layer_fps_,
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
    enqueue_job(PrintVideoJob{PrintVideoJobKind::FinishSave, 0, 0});
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
        case PrintVideoJobKind::FinishSave:
            finalize_session(false);
            break;
        case PrintVideoJobKind::AbortCleanup:
            finalize_session(true);
            break;
        }
    }
}

bool PrintVideoRecorder::grab_latest_nv12(std::vector<uint8_t>& out)
{
    if(!snapshot_provider_) {
        return false;
    }
    return snapshot_provider_(out);
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

std::string PrintVideoRecorder::frame_png_path(uint32_t frame_index) const
{
    char name[64];
    std::snprintf(name, sizeof(name), "%u.png", frame_index);
    return session_dir_ + "/" + name;
}

bool PrintVideoRecorder::encode_nv12_to_png(const uint8_t* nv12,
                                            size_t bytes,
                                            uint32_t frame_index)
{
    if(!nv12 || session_dir_.empty()) {
        return false;
    }
    const size_t need =
        static_cast<size_t>(stream_width_) * stream_height_ * 3u / 2u;
    if(bytes < need) {
        return false;
    }

    const std::string png_path = frame_png_path(frame_index);
    const std::string tmp_nv12_path = session_dir_ + "/.tmp_" +
        std::to_string(frame_index) + ".nv12";

    FILE* fp = std::fopen(tmp_nv12_path.c_str(), "wb");
    if(!fp) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_video open temp nv12 fail %s: %s",
                               tmp_nv12_path.c_str(), std::strerror(errno));
        return false;
    }
    const bool write_ok = std::fwrite(nv12, 1, need, fp) == need;
    if(std::fclose(fp) != 0 || !write_ok) {
        UPC_LOG_CAMERA_SERVICE(Error, "print_video write temp nv12 fail %s",
                               tmp_nv12_path.c_str());
        (void)std::remove(tmp_nv12_path.c_str());
        return false;
    }

    char cmd[1280];
    std::snprintf(
        cmd, sizeof(cmd),
        "%s -y -loglevel error -f rawvideo -pix_fmt nv12 -s %ux%u -i '%s' "
        "-frames:v 1 -c:v png '%s'",
        CAMERA_FFMPEG_BIN, stream_width_, stream_height_,
        tmp_nv12_path.c_str(), png_path.c_str());
    const bool ok = run_shell_cmd(cmd, "ffmpeg png") == 0;
    (void)std::remove(tmp_nv12_path.c_str());
    if(!ok) {
        (void)std::remove(png_path.c_str());
    }
    return ok;
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

    const uint32_t frame_index = frame_count_ + 1u;
    if(!encode_nv12_to_png(nv12.data(), nv12.size(), frame_index)) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_video png encode fail session=%s layer=%u frame=%u",
            session_id_.c_str(), job.layer_index, frame_index);
        return false;
    }
    const size_t nv12_bytes = nv12.size();
    ++frame_count_;
    last_layer_nv12_ = std::move(nv12);
    UPC_LOG_CAMERA_SERVICE(
        Info,
        "print_video snap ok session=%s layer=%u frame=%u height_um=%u nv12_bytes=%zu path=%s",
        session_id_.c_str(),
        job.layer_index,
        frame_index,
        static_cast<unsigned>(job.layer_height_um),
        nv12_bytes,
        frame_png_path(frame_index).c_str());
    return true;
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

void PrintVideoRecorder::finalize_session(bool aborted)
{
    const uint32_t frames = frame_count_;
    if(frames == 0) {
        UPC_LOG_CAMERA_SERVICE(
            Warn,
            "print_video %s with no frames session=%s (no manifest)",
            aborted ? "abort" : "finish",
            session_id_.c_str());
        remove_session_dir();
        session_id_.clear();
        session_name_.clear();
        session_dir_.clear();
        start_unix_ = 0;
        frame_count_ = 0;
        return;
    }

    const std::string thumb_rel = session_name_ + "/thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT;
    const std::string thumb_path = session_dir_ + "/thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT;
    (void)std::remove(thumb_path.c_str());

    bool thumb_ok = false;
    if(!last_layer_nv12_.empty()) {
        thumb_ok = create_raw_thumb_from_nv12(last_layer_nv12_, thumb_path.c_str());
    }
    if(!thumb_ok) {
        UPC_LOG_CAMERA_SERVICE(Warn, "print_video thumb generation failed path=%s",
                               thumb_path.c_str());
    }

    upsert_print_history(aborted, session_name_, thumb_ok ? thumb_rel : std::string{});
    UPC_LOG_CAMERA_SERVICE(
        Info,
        "print_video saved -> %s frames=%u thumb=%s%s",
        session_dir_.c_str(),
        frames,
        thumb_ok ? thumb_path.c_str() : "(none)",
        aborted ? " aborted" : "");

    session_id_.clear();
    session_name_.clear();
    session_dir_.clear();
    start_unix_ = 0;
    frame_count_ = 0;
    last_layer_nv12_.clear();
}

bool PrintVideoRecorder::remove_session_dir()
{
    if(session_dir_.empty()) {
        return true;
    }
    std::error_code ec;
    (void)std::filesystem::remove_all(session_dir_, ec);
    return !std::filesystem::exists(session_dir_, ec);
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
        const unsigned char uc = static_cast<unsigned char>(c);
        if(!std::isalnum(uc) && c != '-' && c != '_' && c != '.') {
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
    bool aborted, const std::string& frames_dir_name,
    const std::string& thumb_name) const
{
    if(frames_dir_name.empty()) {
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
    entry.frames_dir = frames_dir_name;
    entry.thumb      = thumb_name;

    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        UPC_LOG_CAMERA_SERVICE(Warn,
            "print_history load fail path=%s (rewrite)",
            UPC_PRINT_VIDEO_MANIFEST);
        entries.clear();
    }
    upc::print_history_manifest_upsert(entries, entry);

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

    UPC_LOG_CAMERA_SERVICE(
        Info,
        "print_history upsert job=%s status=%s frames_dir=%s duration=%u material_g=%.2f",
        entry.job_name.c_str(),
        upc::print_history_status_str(entry.status),
        entry.frames_dir.c_str(),
        entry.duration_sec,
        static_cast<double>(entry.material_g));
}

} // namespace upc::camera
