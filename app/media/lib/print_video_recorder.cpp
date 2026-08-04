#include "print_video_recorder.hpp"

#include "device_config.hpp"
#include "gcode_manifest.hpp"
#include "msg_port.hpp"
#include "print_history_manifest.hpp"
#include "rs_log.hpp"
#include "timelapse_util.hpp"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

namespace upc::media {

namespace {

bool remove_empty_dir_if_exists(const std::string& dir)
{
    if(dir.empty()) {
        return false;
    }
    std::error_code ec;
    if(!std::filesystem::is_directory(dir, ec)) {
        return false;
    }
    if(!std::filesystem::is_empty(dir, ec)) {
        return false;
    }
    return std::filesystem::remove(dir, ec);
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

void PrintVideoRecorder::set_timelapse_fps(uint32_t fps)
{
    if(fps > 0) {
        layer_fps_ = fps;
    }
}

void PrintVideoRecorder::set_layer_snap_enabled(bool enabled)
{
    layer_snap_enabled_ = enabled;
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
    h264_continuous_on_ = (info.h264_continuous_on != 0);
    /* 同一 gcode 重复打印时 finalize 会覆盖同名正式目录；打印中统一写 tmp/ */
    session_name_ = job_name_.empty() ? std::string("print") : job_name_;
    if(session_name_ == UPC_VIDEO_TMP_NAME) {
        session_name_ = "print_tmp";
        job_name_ = session_name_;
    }
    session_final_dir_ = std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + session_name_;
    session_dir_ = UPC_VIDEO_TMP;

    mkdir_p(CAMERA_PRINT_VIDEO_OUT_DIR);
    remove_session_dir();
    if(mkdir(session_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
        UPC_LOG_MEDIA_SERVICE(Error, "print_video mkdir fail %s: %s",
                               session_dir_.c_str(), std::strerror(errno));
        session_active_ = false;
        return;
    }

    session_active_ = true;
    UPC_LOG_MEDIA_SERVICE(
        Info,
        "print_video session start id=%s job=%s dir=%s %ux%u layer_fps=%u h264=%d snap=%d start_unix=%lld",
        session_id_.c_str(),
        job_name_.c_str(),
        session_dir_.c_str(),
        stream_width_,
        stream_height_,
        layer_fps_,
        h264_continuous_on_ ? 1 : 0,
        layer_snap_enabled_ ? 1 : 0,
        static_cast<long long>(start_unix_));
}

void PrintVideoRecorder::on_layer_snap(
    const middleware::PrintVideoLayerSnapPayload& snap)
{
    if(!session_active_ || !layer_snap_enabled_ || snap.session_id[0] == '\0' ||
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

void PrintVideoRecorder::set_sender_module_id(middleware::ModuleId module_id)
{
    sender_module_id_ = module_id;
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
    UPC_LOG_MEDIA_SERVICE(Info, "print_video %s: %s",
                           log_tag ? log_tag : "cmd", cmd);
    const int rc = std::system(cmd);
    if(rc != 0) {
        UPC_LOG_MEDIA_SERVICE(Error,
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
    char cmd[1280];
    std::snprintf(
        cmd, sizeof(cmd),
        "%s -y -loglevel error -f rawvideo -pix_fmt nv12 -s %ux%u -i - "
        "-frames:v 1 -c:v png '%s'",
        CAMERA_FFMPEG_BIN, stream_width_, stream_height_, png_path.c_str());

    FILE* ff = ::popen(cmd, "w");
    if(!ff) {
        UPC_LOG_MEDIA_SERVICE(Error, "print_video open ffmpeg pipe fail frame=%u",
                               frame_index);
        return false;
    }
    const bool write_ok = (std::fwrite(nv12, 1, need, ff) == need);
    const int rc = ::pclose(ff);
    const bool ok = write_ok && (rc == 0);
    if(!ok) {
        if(!write_ok) {
            UPC_LOG_MEDIA_SERVICE(Error, "print_video write frame to ffmpeg fail frame=%u",
                                   frame_index);
        } else {
            UPC_LOG_MEDIA_SERVICE(Error, "print_video ffmpeg png failed rc=%d frame=%u",
                                   rc, frame_index);
        }
        (void)std::remove(png_path.c_str());
    }
    return ok;
}

bool PrintVideoRecorder::handle_layer_snap(const PrintVideoJob& job)
{
    std::vector<uint8_t> nv12;
    if(!grab_latest_nv12(nv12)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_video snap miss SHM frame session=%s layer=%u",
            session_id_.c_str(), job.layer_index);
        return false;
    }

    const uint32_t frame_index = frame_count_ + 1u;
    if(!encode_nv12_to_png(nv12.data(), nv12.size(), frame_index)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_video png encode fail session=%s layer=%u frame=%u",
            session_id_.c_str(), job.layer_index, frame_index);
        return false;
    }
    const size_t nv12_bytes = nv12.size();
    ++frame_count_;
    last_layer_nv12_ = std::move(nv12);
    UPC_LOG_MEDIA_SERVICE(
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
    /* 统一走 timelapse_util（dma32 + importbuffer_fd），避免堆 VA 触发 RGA >4G 失败 */
    return timelapse_util::create_raw_thumb_from_nv12(
        nv12, stream_width_, stream_height_, out_path);
}

void PrintVideoRecorder::finalize_session(bool aborted)
{
    const uint32_t frames = frame_count_;
    if(frames == 0) {
        UPC_LOG_MEDIA_SERVICE(
            Warn,
            "print_video %s with no frames session=%s (no manifest)",
            aborted ? "abort" : "finish",
            session_id_.c_str());
        /* 仅在目录为空时做轻量清理：避免留下空 __work 目录；
         * 非空目录（可能有 H264 后台产物）保持不动。 */
        if(remove_empty_dir_if_exists(session_dir_)) {
            UPC_LOG_MEDIA_SERVICE(
                Info, "print_video removed empty work dir=%s", session_dir_.c_str());
        }
        notify_manifest_ready(std::string{}, false, aborted, false);
        session_id_.clear();
        session_name_.clear();
        session_final_dir_.clear();
        session_dir_.clear();
        start_unix_ = 0;
        frame_count_ = 0;
        h264_continuous_on_ = false;
        return;
    }

    const std::string thumb_path = session_dir_ + "/thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT;
    (void)std::remove(thumb_path.c_str());

    bool thumb_ok = false;
    if(!last_layer_nv12_.empty()) {
        thumb_ok = create_raw_thumb_from_nv12(last_layer_nv12_, thumb_path.c_str());
    }
    if(!thumb_ok) {
        UPC_LOG_MEDIA_SERVICE(Warn, "print_video thumb generation failed path=%s",
                               thumb_path.c_str());
    }

    std::string frames_dir_name = UPC_VIDEO_TMP_NAME;
    const std::string thumb_rel = std::string(UPC_VIDEO_TMP_NAME) +
                                  "/thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT;

    const bool need_video = (frames >= 1u);
    /* 仅当本次会话开了连续 H264 时，Print History 才进入「生成中」；关则不落 print 视频项。 */
    const bool need_print_video = need_video && h264_continuous_on_;

    /* 快路径：先落盘 manifest（含统计与缩略图），UI 可立即切结果页。 */
    const bool manifest_quick_ok = upsert_print_history(
        aborted,
        frames_dir_name,
        thumb_ok ? thumb_rel : std::string{},
        std::string{},
        need_video,
        need_print_video);
    notify_manifest_ready(frames_dir_name, manifest_quick_ok, aborted, false);
    if(!need_video) {
        UPC_LOG_MEDIA_SERVICE(Info, "print_video skip mp4: frames=%u < 1 session=%s",
                               frames, session_id_.c_str());
    }

    UPC_LOG_MEDIA_SERVICE(
        Info,
        "print_video saved work -> %s frames=%u thumb=%s video_pending=%d print_video_pending=%d%s",
        session_dir_.c_str(),
        frames,
        thumb_ok ? thumb_path.c_str() : "(none)",
        need_video ? 1 : 0,
        need_print_video ? 1 : 0,
        aborted ? " aborted" : "");

    session_id_.clear();
    session_name_.clear();
    session_final_dir_.clear();
    session_dir_.clear();
    start_unix_ = 0;
    frame_count_ = 0;
    h264_continuous_on_ = false;
    last_layer_nv12_.clear();
}

bool PrintVideoRecorder::commit_after_exit(const char* frames_dir)
{
    if(!frames_dir || !frames_dir[0]) {
        return false;
    }
    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_video commit load fail path=%s", UPC_PRINT_VIDEO_MANIFEST);
        return false;
    }

    upc::PrintHistoryEntry* hit = nullptr;
    for(auto& e : entries) {
        if(e.frames_dir == frames_dir) {
            hit = &e;
            break;
        }
    }
    if(!hit) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_video commit miss frames_dir=%s", frames_dir);
        return false;
    }
    hit->committed = true;

    const std::string tmp_path = std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp_path, entries)) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "print_video commit save fail tmp=%s", tmp_path.c_str());
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    if(std::rename(tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "print_video commit rename fail %s -> %s: %s",
            tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST, std::strerror(errno));
        (void)std::remove(tmp_path.c_str());
        return false;
    }

    const bool aborted = (hit->status == upc::PrintHistoryStatus::Terminated);
    const bool has_video = !hit->video.empty();
    notify_manifest_ready(hit->frames_dir, true, aborted, has_video);
    return true;
}

void PrintVideoRecorder::finalize_after_exit(const char* frames_dir)
{
    if(!frames_dir || !frames_dir[0]) {
        return;
    }

    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_video finalize load fail path=%s",
            UPC_PRINT_VIDEO_MANIFEST);
        return;
    }

    upc::PrintHistoryEntry* hit = nullptr;
    for(auto& e : entries) {
        if(e.frames_dir == frames_dir) {
            hit = &e;
            break;
        }
    }
    if(!hit) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_video finalize miss frames_dir=%s", frames_dir);
        return;
    }

    std::string session_dir =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + hit->frames_dir;
    std::string final_frames_dir = sanitize_filename(hit->job_name.c_str());
    if(final_frames_dir.empty()) {
        final_frames_dir = hit->frames_dir;
    }
    const std::string final_session_dir =
        std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + final_frames_dir;
    if(hit->frames_dir != final_frames_dir) {
        std::error_code ec;
        std::filesystem::remove_all(final_session_dir, ec);
        ec.clear();
        std::filesystem::rename(session_dir, final_session_dir, ec);
        if(ec) {
            UPC_LOG_MEDIA_SERVICE(Warn,
                "print_video finalize promote fail %s -> %s: %s",
                session_dir.c_str(), final_session_dir.c_str(), ec.message().c_str());
        } else {
            session_dir = final_session_dir;
            hit->frames_dir = final_frames_dir;
            hit->thumb = final_frames_dir + "/thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT;
            if(!hit->print_thumb.empty()) {
                hit->print_thumb = final_frames_dir + "/print_thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT;
            }
        }
    }
    const uint32_t frame_count = timelapse_util::scan_max_png_index(session_dir);
    const uint32_t fps = layer_fps_ > 0 ? layer_fps_ : 1u;
    const std::string mp4_path = session_dir + "/timelapse.mp4";
    const std::string mp4_rel = hit->frames_dir + "/timelapse.mp4";

    bool video_ok = false;
    if(frame_count >= 1u) {
        (void)std::remove(mp4_path.c_str());
        video_ok = timelapse_util::encode_png_sequence_to_mp4(
            session_dir, frame_count, fps, mp4_path.c_str());
        if(!video_ok) {
            UPC_LOG_MEDIA_SERVICE(Warn,
                "print_video finalize mp4 generation failed dir=%s",
                session_dir.c_str());
        }
    } else {
        UPC_LOG_MEDIA_SERVICE(Info,
            "print_video finalize skip mp4: frames=%u dir=%s",
            frame_count, session_dir.c_str());
    }

    if(video_ok) {
        hit->video = mp4_rel;
        if(hit->clean_first_png > 0 &&
           hit->clean_last_png >= hit->clean_first_png) {
            timelapse_util::remove_png_range(
                session_dir, hit->clean_first_png, hit->clean_last_png);
        }
    }

    const std::string h264_path = session_dir + "/print.h264";
    const std::string h264_mp4_path = session_dir + "/print_h264.mp4";
    const std::string h264_mp4_rel = hit->frames_dir + "/print_h264.mp4";
    const std::string print_thumb_path = session_dir + "/print_thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT;
    const std::string print_thumb_rel =
        hit->frames_dir + "/print_thumb" + CAMERA_PRINT_VIDEO_THUMB_EXT;
    hit->print_video.clear();
    hit->print_thumb.clear();
    (void)std::remove(h264_mp4_path.c_str());
    std::error_code h264_ec;
    bool h264_remux_ok = false;
    if(std::filesystem::exists(h264_path, h264_ec) && !h264_ec) {
        const bool h264_mp4_ok = timelapse_util::remux_h264_to_mp4(
            h264_path.c_str(), fps, h264_mp4_path.c_str());
        if(!h264_mp4_ok) {
            UPC_LOG_MEDIA_SERVICE(
                Warn, "print_video finalize h264 remux failed path=%s",
                h264_path.c_str());
        } else {
            h264_remux_ok = true;
            hit->print_video = h264_mp4_rel;
            if(!hit->thumb.empty()) {
                const std::string timelapse_thumb_path =
                    std::string(CAMERA_PRINT_VIDEO_OUT_DIR) + "/" + hit->thumb;
                std::error_code copy_ec;
                (void)std::filesystem::remove(print_thumb_path, copy_ec);
                copy_ec.clear();
                std::filesystem::copy_file(
                    timelapse_thumb_path, print_thumb_path,
                    std::filesystem::copy_options::overwrite_existing, copy_ec);
                if(copy_ec) {
                    UPC_LOG_MEDIA_SERVICE(
                        Warn,
                        "print_video finalize copy print_thumb failed %s -> %s: %s",
                        timelapse_thumb_path.c_str(),
                        print_thumb_path.c_str(),
                        copy_ec.message().c_str());
                } else {
                    hit->print_thumb = print_thumb_rel;
                }
            }
        }
    }
    if(h264_remux_ok) {
        if(std::remove(h264_path.c_str()) != 0 && errno != ENOENT) {
            UPC_LOG_MEDIA_SERVICE(
                Warn, "print_video finalize remove h264 failed path=%s err=%s",
                h264_path.c_str(), std::strerror(errno));
        }
    } else if(std::filesystem::exists(h264_path, h264_ec) && !h264_ec) {
        UPC_LOG_MEDIA_SERVICE(
            Warn, "print_video finalize keep h264 for retry path=%s",
            h264_path.c_str());
    }

    hit->video_pending = false;
    hit->print_video_pending = false;
    hit->clean_first_png = 0;
    hit->clean_last_png = 0;

    /* 同名覆盖：把“旧正式条目 + 本次 __work 条目”合并为一条最终条目。 */
    const upc::PrintHistoryEntry merged_entry = *hit;
    const std::string work_frames_dir = frames_dir ? std::string(frames_dir) : std::string{};
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
            [&](const upc::PrintHistoryEntry& e) {
                return (!merged_entry.frames_dir.empty() &&
                        e.frames_dir == merged_entry.frames_dir) ||
                       (!work_frames_dir.empty() &&
                        e.frames_dir == work_frames_dir);
            }),
        entries.end());
    entries.push_back(merged_entry);

    const std::string tmp_path =
        std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp_path, entries)) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "print_video finalize save fail tmp=%s", tmp_path.c_str());
        (void)std::remove(tmp_path.c_str());
        return;
    }
    if(std::rename(tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "print_video finalize rename fail %s -> %s: %s",
            tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST, std::strerror(errno));
        (void)std::remove(tmp_path.c_str());
        return;
    }

    const bool aborted = (hit->status == upc::PrintHistoryStatus::Terminated);
    notify_manifest_ready(hit->frames_dir, true, aborted, video_ok);
    UPC_LOG_MEDIA_SERVICE(
        Info,
        "print_video finalize done frames_dir=%s frames=%u video_ok=%d fps=%u",
        hit->frames_dir.c_str(), frame_count, video_ok ? 1 : 0, fps);
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

bool PrintVideoRecorder::promote_session_dir()
{
    if(session_dir_.empty() || session_final_dir_.empty()) {
        return false;
    }
    if(session_dir_ == session_final_dir_) {
        return true;
    }
    std::error_code ec;
    std::filesystem::remove_all(session_final_dir_, ec);
    ec.clear();
    std::filesystem::rename(session_dir_, session_final_dir_, ec);
    if(ec) {
        UPC_LOG_MEDIA_SERVICE(
            Error,
            "print_video promote dir failed %s -> %s: %s",
            session_dir_.c_str(),
            session_final_dir_.c_str(),
            ec.message().c_str());
        return false;
    }
    session_dir_ = session_final_dir_;
    return true;
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

bool PrintVideoRecorder::upsert_print_history(
    bool aborted, const std::string& frames_dir_name,
    const std::string& thumb_name, const std::string& video_name,
    bool video_pending, bool print_video_pending) const
{
    if(frames_dir_name.empty()) {
        return false;
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
    entry.video      = video_name;
    entry.video_pending = video_pending;
    entry.print_video_pending = print_video_pending;
    entry.committed = false;
    entry.clean_first_png = 0;
    entry.clean_last_png = 0;

    std::vector<upc::PrintHistoryEntry> entries;
    if(!upc::print_history_manifest_load(UPC_PRINT_VIDEO_MANIFEST, entries)) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "print_history load fail path=%s (rewrite)",
            UPC_PRINT_VIDEO_MANIFEST);
        entries.clear();
    }
    for(const auto& e : entries) {
        if(e.frames_dir == entry.frames_dir) {
            entry.committed = e.committed;
            break;
        }
    }
    upc::print_history_manifest_upsert(entries, entry);

    const std::string tmp_path =
        std::string(UPC_PRINT_VIDEO_MANIFEST) + ".tmp";
    if(!upc::print_history_manifest_save(tmp_path, entries)) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "print_history save fail tmp=%s", tmp_path.c_str());
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    if(std::rename(tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST) != 0) {
        UPC_LOG_MEDIA_SERVICE(Error,
            "print_history rename fail %s -> %s: %s",
            tmp_path.c_str(), UPC_PRINT_VIDEO_MANIFEST, std::strerror(errno));
        (void)std::remove(tmp_path.c_str());
        return false;
    }

    UPC_LOG_MEDIA_SERVICE(
        Info,
        "print_history upsert job=%s status=%s frames_dir=%s duration=%u material_g=%.2f",
        entry.job_name.c_str(),
        upc::print_history_status_str(entry.status),
        entry.frames_dir.c_str(),
        entry.duration_sec,
        static_cast<double>(entry.material_g));
    return true;
}

void PrintVideoRecorder::notify_manifest_ready(const std::string& frames_dir_name,
                                               bool has_entry, bool aborted,
                                               bool has_video) const
{
    middleware::PrintVideoManifestPayload ntf{};
    if(has_entry && !frames_dir_name.empty()) {
        std::snprintf(ntf.frames_dir, sizeof(ntf.frames_dir), "%s",
                      frames_dir_name.c_str());
    }
    ntf.has_entry = has_entry ? 1 : 0;
    ntf.aborted   = aborted ? 1 : 0;
    ntf.has_video = has_video ? 1 : 0;

    middleware::Message msg(
        middleware::MsgType::APP_Print_Video_Manifest_Ntf,
        sender_module_id_,
        middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&ntf), sizeof(ntf));
    middleware::MsgPort::instance().publish_async(msg);

    UPC_LOG_MEDIA_SERVICE(
        Info,
        "print_video manifest ntf -> UI frames_dir=%s entry=%d aborted=%d video=%d",
        ntf.frames_dir[0] ? ntf.frames_dir : "(none)",
        ntf.has_entry, ntf.aborted, ntf.has_video);
}

} // namespace upc::media
