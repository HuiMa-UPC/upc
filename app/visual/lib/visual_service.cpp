#include "visual_service.hpp"

#include "generated_fusion_model.hpp"
#include "generated_rknn_contract.hpp"
#include "media_paths.hpp"
#include "rs_log.hpp"
#include "upc_contract.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <utility>

#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace upc::visual {

namespace {

/* 起流重试间隔（ms）：避免 Camera 未就绪时高频打消息。 */
constexpr uint64_t kStartRetryMs = 1000;
constexpr std::size_t kTemporalFrameCount = 5;
constexpr uint32_t kTemporalFrameStride = 3;
constexpr double kTemporalDirectRejectProbability = 0.45;
constexpr double kTemporalDirectAcceptProbability = 0.70;

bool temporal_detection_usable(const FirstLayerDetection& detection)
{
    if(!detection.error.empty() || !detection.alignment_passed ||
       !detection.cache_available || detection.is_ood ||
       detection.confidence < fusion_model::kConfidenceThreshold) {
        return false;
    }
    return detection.rejection_reasons.empty() ||
        detection.rejection_reasons == "ready_probability_below_threshold";
}

struct DiagnosticImageSaveResult {
    bool saved = false;
    std::string path = "none";
    std::string error = "none";
    double elapsed_ms = 0.0;
};

void close_dma_fds(std::vector<int>& fds)
{
    for(int fd : fds) {
        if(fd >= 0) {
            ::close(fd);
        }
    }
    fds.clear();
}

std::string diagnostic_image_dir(const char* environment_name, const char* default_path)
{
    const char* value = std::getenv(environment_name);
    return value && value[0] != '\0' ? value : default_path;
}

std::string qualified_image_dir()
{
    return diagnostic_image_dir("FIRST_LAYER_QUALIFIED_IMAGE_DIR",
                                rknn_first_layer_contract::kQualifiedImageDir);
}

std::string rejected_image_dir()
{
    return diagnostic_image_dir("FIRST_LAYER_REJECTED_IMAGE_DIR",
                                UPC_FLV_REJECTED_DIR);
}

bool ensure_directory_tree(const std::string& path, std::string& error)
{
    if(path.empty() || path.front() != '/') {
        error = "diagnostic_image_dir_must_be_absolute";
        return false;
    }
    std::string current;
    std::size_t begin = 1;
    while(begin <= path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string component = path.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if(!component.empty()) {
            current += "/" + component;
            if(::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                error = "mkdir_errno_" + std::to_string(errno);
                return false;
            }
        }
        if(end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

bool has_diagnostic_image_name(const std::string& name, const char* prefix)
{
    constexpr const char* suffix = ".jpg";
    return name.rfind(prefix, 0) == 0 && name.size() > std::strlen(suffix) &&
        name.compare(name.size() - std::strlen(suffix), std::strlen(suffix), suffix) == 0;
}

void prune_diagnostic_images(const std::string& directory, const char* prefix)
{
    DIR* handle = ::opendir(directory.c_str());
    if(!handle) {
        return;
    }
    std::vector<std::string> names;
    while(dirent* entry = ::readdir(handle)) {
        const std::string name(entry->d_name);
        if(has_diagnostic_image_name(name, prefix)) {
            names.push_back(name);
        }
    }
    ::closedir(handle);
    std::sort(names.begin(), names.end());
    const std::size_t limit = rknn_first_layer_contract::kMaxQualifiedImages;
    if(names.size() <= limit) {
        return;
    }
    for(std::size_t index = 0; index < names.size() - limit; ++index) {
        const std::string path = directory + "/" + names[index];
        (void)::unlink(path.c_str());
    }
}

std::string diagnostic_image_filename(const char* prefix, uint32_t frame_seq,
                                      double ready_probability)
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    (void)::localtime_r(&timestamp, &local);
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer),
        "%s_%04d%02d%02d_%02d%02d%02d_%03lld_frame_%u_p%.4f.jpg", prefix,
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
        local.tm_hour, local.tm_min, local.tm_sec,
        static_cast<long long>(milliseconds), static_cast<unsigned>(frame_seq),
        ready_probability);
    return buffer;
}

DiagnosticImageSaveResult save_diagnostic(
    const std::vector<uint8_t>& nv12, uint32_t width, uint32_t height,
    uint32_t frame_seq, const FirstLayerDetection& detection,
    const std::string& directory, const char* prefix)
{
    DiagnosticImageSaveResult result;
    const auto begin = std::chrono::steady_clock::now();
    try {
        const std::size_t canonical_bytes =
            static_cast<std::size_t>(detection.canonical_width) *
            static_cast<std::size_t>(detection.canonical_height);
        if(detection.canonical_width == 0 || detection.canonical_height == 0 ||
           detection.canonical_gray.size() != canonical_bytes) {
            result.error = "canonical_image_unavailable";
            return result;
        }

        std::string directory_error;
        if(!ensure_directory_tree(directory, directory_error)) {
            result.error = directory_error;
            return result;
        }

        cv::Mat nv12_view(static_cast<int>(height * 3u / 2u),
                          static_cast<int>(width), CV_8UC1,
                          const_cast<uint8_t*>(nv12.data()));
        cv::Mat camera_bgr;
        cv::cvtColor(nv12_view, camera_bgr, cv::COLOR_YUV2BGR_NV12);
        const double camera_scale = std::min(1.0, 720.0 / camera_bgr.rows);
        cv::Mat camera_panel;
        cv::resize(camera_bgr, camera_panel, cv::Size(), camera_scale,
                   camera_scale, cv::INTER_AREA);

        cv::Mat canonical_gray(
            static_cast<int>(detection.canonical_height),
            static_cast<int>(detection.canonical_width), CV_8UC1,
            const_cast<uint8_t*>(detection.canonical_gray.data()));
        cv::Mat canonical_bgr;
        cv::cvtColor(canonical_gray, canonical_bgr, cv::COLOR_GRAY2BGR);
        cv::Mat canonical_panel;
        cv::resize(canonical_bgr, canonical_panel, cv::Size(560, 560),
                   0.0, 0.0, cv::INTER_NEAREST);

        constexpr int kHeader = 52;
        constexpr int kGap = 16;
        constexpr int kFooter = 88;
        const int panel_height = std::max(camera_panel.rows, canonical_panel.rows);
        cv::Mat canvas(panel_height + kHeader + kFooter,
                       camera_panel.cols + kGap + canonical_panel.cols,
                       CV_8UC3, cv::Scalar(24, 24, 24));
        camera_panel.copyTo(canvas(cv::Rect(0, kHeader,
            camera_panel.cols, camera_panel.rows)));
        canonical_panel.copyTo(canvas(cv::Rect(camera_panel.cols + kGap, kHeader,
            canonical_panel.cols, canonical_panel.rows)));
        cv::putText(canvas, "Camera frame used for detection", cv::Point(12, 34),
                    cv::FONT_HERSHEY_SIMPLEX, 0.75, cv::Scalar(240, 240, 240), 2);
        cv::putText(canvas, "Perspective-corrected model ROI",
                    cv::Point(camera_panel.cols + kGap + 12, 34),
                    cv::FONT_HERSHEY_SIMPLEX, 0.75, cv::Scalar(240, 240, 240), 2);
        char metrics[256]{};
        std::snprintf(metrics, sizeof(metrics),
            "frame=%u ready=%.4f threshold=%.2f confidence=%.4f total=%.1fms",
            static_cast<unsigned>(frame_seq), detection.ready_probability,
            static_cast<double>(fusion_model::kReadyThreshold),
            detection.confidence, detection.total_ms);
        cv::putText(canvas, metrics,
                    cv::Point(12, panel_height + kHeader + 34),
                    cv::FONT_HERSHEY_SIMPLEX, 0.72, cv::Scalar(255, 255, 255), 2);
        const std::string geometry = "cache=" + detection.cache_event +
            " alignment=" + (detection.alignment_passed ? "1" : "0") +
            " segmentation=" + (detection.segmentation_ran ? "1" : "0");
        cv::putText(canvas, geometry,
                    cv::Point(12, panel_height + kHeader + 70),
                    cv::FONT_HERSHEY_SIMPLEX, 0.67, cv::Scalar(220, 220, 220), 2);

        result.path = directory + "/" +
            diagnostic_image_filename(prefix, frame_seq, detection.ready_probability);
        const std::vector<int> parameters{cv::IMWRITE_JPEG_QUALITY, 92};
        result.saved = cv::imwrite(result.path, canvas, parameters);
        if(!result.saved) {
            result.error = "opencv_imwrite_returned_false";
            result.path = "none";
        } else {
            result.error = "none";
            prune_diagnostic_images(directory, prefix);
        }
    } catch(const std::exception& error) {
        result.saved = false;
        result.path = "none";
        result.error = error.what();
    }
    result.elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
    return result;
}

} // namespace

VisualService::~VisualService()
{
    shutdown();
}

bool VisualService::init()
{
    /* 进程常驻但默认休眠；打印开始通知到达后才向 Camera 起流。 */
    subscribe_messages();
    UPC_LOG_VISUAL_SERVICE(Info,
        "first layer session idle camera_stream=0 segmentation=0 inference=0");
    UPC_LOG_VISUAL_SERVICE(Info,
        "first layer model report_v=1 rknn_contract=%s segmentation_sha256=%s "
        "fusion_sha256=%s ready_threshold=%.6f confidence_threshold=%.6f "
        "ood_threshold=%.6f inset_scale=%.6f audit_interval_frames=%u "
        "qualified_image_dir=%s rejected_image_dir=%s max_diagnostic_images=%u",
        rknn_first_layer_contract::kContractVersion,
        rknn_first_layer_contract::kSegmentationSha256,
        rknn_first_layer_contract::kFusionSha256,
        static_cast<double>(fusion_model::kReadyThreshold),
        static_cast<double>(fusion_model::kConfidenceThreshold),
        static_cast<double>(fusion_model::kOodThreshold),
        static_cast<double>(rknn_first_layer_contract::kInsetScale),
        static_cast<unsigned>(rknn_first_layer_contract::kAuditIntervalFrames),
        qualified_image_dir().c_str(),
        rejected_image_dir().c_str(),
        static_cast<unsigned>(rknn_first_layer_contract::kMaxQualifiedImages));
    return true;
}

void VisualService::shutdown()
{
    stop_optical_path_session("shutdown");
    stop_first_layer_session("shutdown");
    subs_.clear();
}

bool VisualService::any_session_active_locked() const
{
    return first_layer_session_active_ || optical_path_session_active_;
}

/**
 * @brief 两类会话都停了才真正拆流
 *
 * 单独给某一类会话调 send_stream_stop_req + stop_worker 会把另一类正要用的流
 * 一起拆掉（并清掉帧快照缓存），所以拆流动作统一收敛到这里。
 */
void VisualService::release_stream_if_idle(const char* reason)
{
    bool had_activity = false;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if(any_session_active_locked()) {
            return;
        }
        had_activity = stream_up_ || start_req_sent_;
        start_req_sent_ = false;
    }

    if(had_activity) {
        send_stream_stop_req();
    }
    stop_worker();
    unmap_dma_views();
    consumer_.close();
    close_dma_fds(dma_fds_);
    clear_latest_snapshot();
    if(had_activity) {
        UPC_LOG_VISUAL_SERVICE(Info, "camera stream released reason=%s", reason);
    }
}

void VisualService::tick()
{
    /* 空闲时此函数不会发起任何 Camera 请求。 */
    send_stream_start_req();
}

void VisualService::subscribe_messages()
{
    auto& bus = middleware::MsgPort::instance();
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Visual_Camera_Stream_Start_Rsp,
        middleware::ModuleId::APP_VISUAL,
        [this](const middleware::Message& msg) { on_stream_start_rsp(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Visual_FirstLayer_Session_Start_Ntf,
        middleware::ModuleId::APP_VISUAL,
        [this](const middleware::Message& msg) {
            on_first_layer_session_start(msg);
        }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Visual_FirstLayer_Session_Stop_Ntf,
        middleware::ModuleId::APP_VISUAL,
        [this](const middleware::Message& msg) {
            on_first_layer_session_stop(msg);
        }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Visual_FirstLayer_Check_Req,
        middleware::ModuleId::APP_VISUAL,
        [this](const middleware::Message& msg) { on_first_layer_check_req(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Visual_OpticalPath_Session_Start_Ntf,
        middleware::ModuleId::APP_VISUAL,
        [this](const middleware::Message& msg) {
            on_optical_path_session_start(msg);
        }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Visual_OpticalPath_Session_Stop_Ntf,
        middleware::ModuleId::APP_VISUAL,
        [this](const middleware::Message& msg) {
            on_optical_path_session_stop(msg);
        }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Visual_OpticalPath_Point_Check_Req,
        middleware::ModuleId::APP_VISUAL,
        [this](const middleware::Message& msg) {
            on_optical_path_point_check_req(msg);
        }));
}

void VisualService::send_stream_start_req()
{
    const uint64_t now = monotonic_ms();
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        /* 首层检测与光路检测共用这一路流：任一会话激活就该起流。 */
        if(!any_session_active_locked() || stream_up_) {
            return;
        }
        if(start_req_sent_ && now - last_start_req_ms_ < kStartRetryMs) {
            return;
        }
        start_req_sent_ = true;
        last_start_req_ms_ = now;
    }

    middleware::Message msg(
        middleware::MsgType::VISUAL_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_VISUAL,
        middleware::ModuleId::APP_CAMERA);
    if(!middleware::MsgPort::instance().publish_async(msg)) {
        std::lock_guard<std::mutex> lock(state_mtx_);
        start_req_sent_ = false;
        UPC_LOG_VISUAL_SERVICE(Warn,
            "first layer camera stream start publish failed");
    }
}

void VisualService::send_stream_stop_req()
{
    middleware::Message msg(
        middleware::MsgType::VISUAL_Camera_Stream_Stop_Req,
        middleware::ModuleId::APP_VISUAL,
        middleware::ModuleId::APP_CAMERA);
    middleware::MsgPort::instance().publish_async(msg);
}

void VisualService::on_first_layer_session_start(const middleware::Message& msg)
{
    (void)msg;
    start_first_layer_session("print_start");
}

void VisualService::on_first_layer_session_stop(const middleware::Message& msg)
{
    (void)msg;
    stop_first_layer_session("glavo_stop");
}

void VisualService::start_first_layer_session(const char* reason)
{
    bool newly_active = false;
    uint64_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if(!first_layer_session_active_) {
            first_layer_session_active_ = true;
            first_layer_session_id_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            first_layer_session_passed_ = false;
            reference_capture_pending_ = true;
            last_reference_capture_attempt_ms_ = 0;
            last_reference_failure_report_ms_ = 0;
            last_reference_failure_reason_.clear();
            newly_active = true;
        }
        session_id = first_layer_session_id_;
    }
    if(newly_active) {
        {
            std::lock_guard<std::mutex> lock(rejected_diagnostic_mtx_);
            latest_rejected_diagnostic_ = RejectedDiagnosticCandidate{};
        }
        {
            std::lock_guard<std::mutex> detector_lock(detector_mtx_);
            first_layer_detector_.clear_bare_reference();
        }
        clear_latest_snapshot();
        UPC_LOG_VISUAL_SERVICE(Info,
            "first layer session start session_id=%llu reason=%s "
            "reference_capture=pending",
            static_cast<unsigned long long>(session_id), reason);
    }
    send_stream_start_req();
}

void VisualService::stop_first_layer_session(const char* reason)
{
    bool had_activity = false;
    bool passed = false;
    uint64_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        had_activity = first_layer_session_active_ || stream_up_ || start_req_sent_;
        passed = first_layer_session_passed_;
        session_id = first_layer_session_id_;
        first_layer_session_active_ = false;
        reference_capture_pending_ = false;
        last_reference_capture_attempt_ms_ = 0;
        last_reference_failure_report_ms_ = 0;
        last_reference_failure_reason_.clear();
    }

    /* 拆流交给 release_stream_if_idle：光路检测会话可能还在用这一路流。 */
    release_stream_if_idle(reason);
    {
        std::lock_guard<std::mutex> detector_lock(detector_mtx_);
        first_layer_detector_.clear_bare_reference();
    }

    if(had_activity) {
        DiagnosticImageSaveResult rejected_image;
        if(!passed) {
            RejectedDiagnosticCandidate candidate;
            {
                std::lock_guard<std::mutex> lock(rejected_diagnostic_mtx_);
                candidate = std::move(latest_rejected_diagnostic_);
                latest_rejected_diagnostic_ = RejectedDiagnosticCandidate{};
            }
            if(candidate.valid) {
                rejected_image = save_diagnostic(
                    candidate.nv12, candidate.width, candidate.height,
                    candidate.frame_seq, candidate.detection, rejected_image_dir(), "rejected");
            } else {
                rejected_image.error = "no_valid_result0_snapshot";
            }
        }
        UPC_LOG_VISUAL_SERVICE(Info,
            "first layer session stop session_id=%llu reason=%s final_result=%d "
            "camera_stream=0 rejected_image_saved=%d rejected_image_path=%s "
            "rejected_image_save_ms=%.3f rejected_image_error=%s",
            static_cast<unsigned long long>(session_id), reason, passed ? 1 : 0,
            rejected_image.saved ? 1 : 0, rejected_image.path.c_str(),
            rejected_image.elapsed_ms, rejected_image.error.c_str());
    }
}

void VisualService::on_optical_path_session_start(const middleware::Message& msg)
{
    (void)msg;
    start_optical_path_session("glavo_start");
}

void VisualService::on_optical_path_session_stop(const middleware::Message& msg)
{
    (void)msg;
    stop_optical_path_session("glavo_stop");
}

void VisualService::start_optical_path_session(const char* reason)
{
    bool newly_active = false;
    uint64_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if(!optical_path_session_active_) {
            optical_path_session_active_ = true;
            optical_path_session_id_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            newly_active = true;
        }
        session_id = optical_path_session_id_;
    }
    if(newly_active) {
        red_dot_detector_.reset_session();
        UPC_LOG_VISUAL_SERVICE(Info,
            "optical path session start session_id=%llu reason=%s",
            static_cast<unsigned long long>(session_id), reason);
    }
    send_stream_start_req();
}

void VisualService::stop_optical_path_session(const char* reason)
{
    bool was_active = false;
    uint64_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        was_active = optical_path_session_active_;
        session_id = optical_path_session_id_;
        optical_path_session_active_ = false;
    }
    stop_optical_point_thread();
    release_stream_if_idle(reason);
    if(was_active) {
        UPC_LOG_VISUAL_SERVICE(Info,
            "optical path session stop session_id=%llu reason=%s",
            static_cast<unsigned long long>(session_id), reason);
    }
}

void VisualService::stop_optical_point_thread()
{
    optical_point_abort_.store(true);
    if(optical_point_thread_.joinable()) {
        optical_point_thread_.join();
    }
    optical_point_abort_.store(false);
}

void VisualService::on_optical_path_point_check_req(const middleware::Message& msg)
{
    middleware::VisualOpticalPathCheckReqPayload req{};
    if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length < sizeof(req)) {
        UPC_LOG_VISUAL_SERVICE(Warn, "optical path check invalid request payload");
        middleware::VisualOpticalPathCheckReqPayload dummy{};
        dummy.session_id = 0;
        dummy.point_index = 0xff;
        send_optical_path_point_rsp(dummy, RedDotDetection{});
        return;
    }
    std::memcpy(&req, msg.payload.data.bytes.data(), sizeof(req));

    bool session_active = false;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        session_active = optical_path_session_active_;
    }
    if(!session_active) {
        /* 会话 Start 丢失时先起流；本点没帧则窗口耗尽后回 result=0。 */
        start_optical_path_session("check_without_start");
    }

    /* 上一点的取点线程理论上已经结束（Glavo 逐点等待），但防御性回收一次。
     * abort 标志由 stop_optical_point_thread 在 join 后自行落回 false。 */
    stop_optical_point_thread();
    optical_point_thread_ = std::thread(
        [this, req]() { run_optical_point_window(req); });
}

void VisualService::run_optical_point_window(
    const middleware::VisualOpticalPathCheckReqPayload& req)
{
    const uint32_t timeout_ms = (req.timeout_ms == 0u) ? 3000u : req.timeout_ms;
    const uint64_t deadline = monotonic_ms() + timeout_ms;
    RedDotHint hint;
    hint.expect_x_mm = static_cast<double>(req.expect_x_mm_x10) / 10.0;
    hint.expect_y_mm = static_cast<double>(req.expect_y_mm_x10) / 10.0;
    hint.point_index = req.point_index;
    hint.point_total = req.point_total;

    RedDotDetection last;
    last.found = false;
    last.reason = "timeout";

    while(!optical_point_abort_.load() && monotonic_ms() < deadline) {
        std::vector<uint8_t> nv12;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frame_seq = 0;
        if(copy_latest_snapshot(nv12, width, height, frame_seq)) {
            last = red_dot_detector_.detect_nv12(
                nv12.data(), nv12.size(), width, height, frame_seq, hint);
            if(last.found) {
                send_optical_path_point_rsp(req, last);
                UPC_LOG_VISUAL_SERVICE(Info,
                    "optical path point found idx=%u seq=%u px=%.1f,%.1f conf=%.3f",
                    static_cast<unsigned>(req.point_index),
                    static_cast<unsigned>(frame_seq),
                    last.center_x, last.center_y, last.confidence);
                return;
            }
        }
        /* 轮询周期 ~33ms：对应 ~30fps 最新帧。sleep 在取点线程上，
         * 不阻塞 Visual 主循环的 tick / IPC 分发。 */
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    if(optical_point_abort_.load()) {
        last.reason = "aborted";
        /* 被 abort 时不回包：Glavo 侧已经走终止路径，迟到回包会被 session 过滤。 */
        UPC_LOG_VISUAL_SERVICE(Info,
            "optical path point aborted idx=%u session=%u",
            static_cast<unsigned>(req.point_index),
            static_cast<unsigned>(req.session_id));
        return;
    }
    send_optical_path_point_rsp(req, last);
    UPC_LOG_VISUAL_SERVICE(Warn,
        "optical path point not found idx=%u session=%u reason=%s",
        static_cast<unsigned>(req.point_index),
        static_cast<unsigned>(req.session_id), last.reason.c_str());
}

void VisualService::send_optical_path_point_rsp(
    const middleware::VisualOpticalPathCheckReqPayload& req,
    const RedDotDetection& detection)
{
    middleware::VisualOpticalPathCheckRspPayload rsp{};
    rsp.result = detection.found ? 1 : 0;
    rsp.point_index = req.point_index;
    rsp.session_id = req.session_id;
    if(detection.found) {
        const auto clamp_i16 = [](double v) -> int16_t {
            if(v < -32768.0) return static_cast<int16_t>(-32768);
            if(v >  32767.0) return static_cast<int16_t>( 32767);
            return static_cast<int16_t>(v);
        };
        rsp.detected_x_px = clamp_i16(detection.center_x);
        rsp.detected_y_px = clamp_i16(detection.center_y);
        const double q8 = detection.confidence * 256.0;
        if(q8 <= 0.0) {
            rsp.confidence_q8 = 0;
        } else if(q8 >= 65535.0) {
            rsp.confidence_q8 = 65535;
        } else {
            rsp.confidence_q8 = static_cast<uint16_t>(q8);
        }
    } else {
        rsp.detected_x_px = -1;
        rsp.detected_y_px = -1;
        rsp.confidence_q8 = 0;
    }
    middleware::Message out(
        middleware::MsgType::APP_Visual_OpticalPath_Point_Check_Rsp,
        middleware::ModuleId::APP_VISUAL,
        middleware::ModuleId::APP_GLAVO);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    middleware::MsgPort::instance().publish_async(out);
}

void VisualService::cache_final_rejected_diagnostic(
    const std::vector<uint8_t>& nv12, uint32_t width, uint32_t height,
    uint32_t frame_seq, const FirstLayerDetection& detection)
{
    std::lock_guard<std::mutex> lock(rejected_diagnostic_mtx_);
    latest_rejected_diagnostic_.valid = true;
    latest_rejected_diagnostic_.nv12 = nv12;
    latest_rejected_diagnostic_.width = width;
    latest_rejected_diagnostic_.height = height;
    latest_rejected_diagnostic_.frame_seq = frame_seq;
    latest_rejected_diagnostic_.detection = detection;
}

void VisualService::send_reference_ready_rsp(bool ready, uint32_t frame_seq,
                                             const std::string& reason)
{
    middleware::VisualFirstLayerReferenceReadyPayload payload{};
    payload.result = ready ? 1 : 0;
    payload.frame_seq = ready ? frame_seq : 0;
    middleware::Message response(
        middleware::MsgType::APP_Visual_FirstLayer_Reference_Ready_Rsp,
        middleware::ModuleId::APP_VISUAL,
        middleware::ModuleId::APP_GLAVO);
    response.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    middleware::MsgPort::instance().publish_async(response);
    if(ready) {
        UPC_LOG_VISUAL_SERVICE(Info,
            "first layer bare reference result=%u frame=%u reason=%s",
            static_cast<unsigned>(payload.result),
            static_cast<unsigned>(payload.frame_seq), reason.c_str());
    } else {
        UPC_LOG_VISUAL_SERVICE(Error,
            "first layer bare reference result=%u frame=%u reason=%s",
            static_cast<unsigned>(payload.result),
            static_cast<unsigned>(payload.frame_seq), reason.c_str());
    }
}

void VisualService::try_capture_bare_reference(const FrameSnapshot& frame)
{
    constexpr uint64_t kReferenceRetryAttemptMs = 500;
    bool pending = false;
    bool should_attempt = false;
    const uint64_t now_ms = monotonic_ms();
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        pending = first_layer_session_active_ && reference_capture_pending_;
        if(pending) {
            if(last_reference_capture_attempt_ms_ == 0 ||
               (now_ms - last_reference_capture_attempt_ms_) >= kReferenceRetryAttemptMs) {
                last_reference_capture_attempt_ms_ = now_ms;
                should_attempt = true;
            }
        }
    }
    if(!pending || !should_attempt) {
        return;
    }

    std::string reason;
    bool captured = false;
    {
        std::lock_guard<std::mutex> detector_lock(detector_mtx_);
        captured = first_layer_detector_.capture_bare_reference_nv12(
            frame.nv12.data(), frame.nv12.size(), frame.width, frame.height,
            frame.frame_seq, reason);
    }
    if(captured) {
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(state_mtx_);
            if(reference_capture_pending_) {
                reference_capture_pending_ = false;
                last_reference_capture_attempt_ms_ = 0;
                last_reference_failure_report_ms_ = 0;
                last_reference_failure_reason_.clear();
                notify = true;
            }
        }
        if(notify) {
            send_reference_ready_rsp(true, frame.frame_seq, reason);
        }
        return;
    }

    constexpr uint64_t kReferenceRetryNotifyMs = 1200;
    bool notify_retry = false;
    std::string notify_reason = reason.empty() ? "reference_capture_retrying" : reason;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        if(reference_capture_pending_) {
            const bool reason_changed = notify_reason != last_reference_failure_reason_;
            const bool interval_due =
                (last_reference_failure_report_ms_ == 0) ||
                (now_ms - last_reference_failure_report_ms_ >= kReferenceRetryNotifyMs);
            if(reason_changed || interval_due) {
                last_reference_failure_report_ms_ = now_ms;
                last_reference_failure_reason_ = notify_reason;
                notify_retry = true;
            }
        }
    }
    if(notify_retry) {
        send_reference_ready_rsp(false, 0, notify_reason);
    }
}

bool VisualService::read_inline_start_rsp(const middleware::Message& msg,
                                          middleware::CameraStreamInfoPayload& out)
{
    if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length < sizeof(out)) {
        return false;
    }
    std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(out));
    return true;
}

void VisualService::on_stream_start_rsp(const middleware::Message& msg)
{
    middleware::CameraStreamInfoPayload info{};
    if(!read_inline_start_rsp(msg, info)) {
        return;
    }
    info.shm_name[sizeof(info.shm_name) - 1] = '\0';

    bool session_active = false;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        /* 必须与 send_stream_start_req 用同一个判据：光路检测会话单独激活时
         * 首层会话是关着的，只看 first_layer_session_active_ 会把自己请来的
         * 起流回执当成野包丢掉（关 fd 不 mmap），取帧线程永远起不来。 */
        session_active = any_session_active_locked();
        if(!session_active) {
            start_req_sent_ = false;
        }
    }
    if(!session_active) {
        std::vector<int> stale_fds;
        if(middleware::MsgPort::instance().take_peer_dma_fds(
               middleware::ModuleId::APP_CAMERA, stale_fds)) {
            close_dma_fds(stale_fds);
        }
        send_stream_stop_req();
        UPC_LOG_VISUAL_SERVICE(Info,
            "ignore camera stream response: no visual session active");
        return;
    }

    /* 收到新起流响应时，先清理旧会话，保证 fd/ring 一致。 */
    stop_worker();
    unmap_dma_views();
    consumer_.close();
    close_dma_fds(dma_fds_);

    if(info.ok == 0 || info.dma_mode == 0) {
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }

    std::vector<int> fds;
    if(!middleware::MsgPort::instance().take_peer_dma_fds(
           middleware::ModuleId::APP_CAMERA, fds)) {
        UPC_LOG_VISUAL_SERVICE(Error, "take dma fds failed");
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }
    if(fds.size() != info.dma_buf_count) {
        close_dma_fds(fds);
        UPC_LOG_VISUAL_SERVICE(Error,
            "visual dma fd count mismatch got=%zu expected=%u",
            fds.size(), info.dma_buf_count);
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }
    if(consumer_.open(info.shm_name) != upc_ipc::IpcShmError::OK) {
        close_dma_fds(fds);
        UPC_LOG_VISUAL_SERVICE(Error, "open shm failed %s", info.shm_name);
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = false;
        start_req_sent_ = false;
        return;
    }

    dma_fds_ = std::move(fds);
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        stream_up_ = true;
        start_req_sent_ = false;
        pending_start_rsp_ = info;
        pending_start_rsp_valid_ = true;
    }
    /* 起流成功后拉起后台取帧线程。 */
    stop_worker_.store(false);
    worker_ = std::thread(&VisualService::worker_thread_fn, this);
}

void VisualService::stop_worker()
{
    stop_worker_.store(true);
    if(worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(state_mtx_);
    stream_up_ = false;
    pending_start_rsp_valid_ = false;
}

void VisualService::worker_thread_fn()
{
    while(!stop_worker_.load(std::memory_order_relaxed)) {
        /* 持续消费最新帧，缓存一份不带 stride padding 的 NV12。 */
        auto frame = consumer_.acquire_latest_frame();
        if(!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const uint32_t frame_seq = frame->seq;
        if(frame->buffer_idx < dma_fds_.size() && map_dma_views_if_needed()) {
            update_latest_snapshot(*frame);
        }
        consumer_.release_slot(frame->slot_index);

        /* 分割只在缓存初始化/周期审计到期时运行，且必须先释放 Camera 槽位。 */
        bool geometry_due = false;
        {
            std::lock_guard<std::mutex> detector_lock(detector_mtx_);
            geometry_due = first_layer_detector_.geometry_observation_due(frame_seq);
        }
        if(geometry_due) {
            std::vector<uint8_t> snapshot;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t snapshot_seq = 0;
            if(copy_latest_snapshot(snapshot, width, height, snapshot_seq) &&
               snapshot_seq == frame_seq) {
                FirstLayerGeometryObservation observation;
                {
                    std::lock_guard<std::mutex> detector_lock(detector_mtx_);
                    observation = first_layer_detector_.observe_nv12(
                        snapshot.data(), snapshot.size(), width, height, snapshot_seq);
                }
                const bool geometry_changed =
                    observation.event != last_geometry_log_event_ ||
                    observation.reason != last_geometry_log_reason_;
                const bool geometry_log_due = geometry_changed ||
                    static_cast<uint32_t>(snapshot_seq - last_geometry_log_frame_) >= 300u;
                if(observation.segmentation_ran && geometry_log_due) {
                    if(observation.cache_available) {
                        UPC_LOG_VISUAL_SERVICE(Info,
                            "first layer geometry frame=%u event=%s reason=%s "
                            "cache=1 segmentation_ms=%.3f",
                            static_cast<unsigned>(snapshot_seq),
                            observation.event.c_str(), observation.reason.c_str(),
                            observation.segmentation_ms);
                    } else {
                        UPC_LOG_VISUAL_SERVICE(Warn,
                            "first layer geometry frame=%u event=%s reason=%s "
                            "cache=0 segmentation_ms=%.3f",
                            static_cast<unsigned>(snapshot_seq),
                            observation.event.c_str(), observation.reason.c_str(),
                            observation.segmentation_ms);
                    }
                    last_geometry_log_frame_ = snapshot_seq;
                    last_geometry_log_event_ = observation.event;
                    last_geometry_log_reason_ = observation.reason;
                }
            }
        }

        bool capture_pending = false;
        {
            std::lock_guard<std::mutex> lock(state_mtx_);
            capture_pending = reference_capture_pending_;
        }
        if(capture_pending) {
            std::vector<FrameSnapshot> frames;
            if(copy_recent_snapshots(frames) && !frames.empty()) {
                try_capture_bare_reference(frames.back());
            }
        }
    }
}

bool VisualService::map_dma_views_if_needed()
{
    if(!dma_maps_.empty()) {
        return true;
    }
    if(dma_fds_.empty() || !pending_start_rsp_valid_ ||
       pending_start_rsp_.stride_y == 0 || pending_start_rsp_.height == 0) {
        return false;
    }
    dma_map_size_ = static_cast<size_t>(pending_start_rsp_.stride_y) *
        static_cast<size_t>(pending_start_rsp_.height) * 3u / 2u;
    dma_maps_.resize(dma_fds_.size(), nullptr);
    for(size_t i = 0; i < dma_fds_.size(); ++i) {
        void* p = ::mmap(nullptr, dma_map_size_, PROT_READ, MAP_SHARED, dma_fds_[i], 0);
        if(p == MAP_FAILED) {
            UPC_LOG_VISUAL_SERVICE(Error, "visual mmap dma failed idx=%zu", i);
            unmap_dma_views();
            return false;
        }
        dma_maps_[i] = static_cast<uint8_t*>(p);
    }
    return true;
}

void VisualService::unmap_dma_views()
{
    if(dma_map_size_ == 0) {
        dma_maps_.clear();
        return;
    }
    for(uint8_t* p : dma_maps_) {
        if(p) {
            ::munmap(p, dma_map_size_);
        }
    }
    dma_maps_.clear();
    dma_map_size_ = 0;
}

void VisualService::copy_nv12_tight(uint8_t* dst, const uint8_t* src_y,
                                    const uint8_t* src_uv, uint32_t src_stride_y,
                                    uint32_t width, uint32_t height)
{
    if(!dst || !src_y || !src_uv || src_stride_y == 0 || width == 0 || height == 0) {
        return;
    }
    const size_t y_size = static_cast<size_t>(width) * static_cast<size_t>(height);
    uint8_t* dst_uv = dst + y_size;
    if(src_stride_y == width) {
        std::memcpy(dst, src_y, y_size);
        std::memcpy(dst_uv, src_uv, y_size / 2u);
        return;
    }
    for(uint32_t y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * width,
                    src_y + static_cast<size_t>(y) * src_stride_y, width);
    }
    const uint32_t uv_h = height / 2u;
    for(uint32_t y = 0; y < uv_h; ++y) {
        std::memcpy(dst_uv + static_cast<size_t>(y) * width,
                    src_uv + static_cast<size_t>(y) * src_stride_y, width);
    }
}

void VisualService::update_latest_snapshot(const upc_ipc::IpcDmaFrameView& frame)
{
    const uint32_t idx = frame.buffer_idx;
    if(idx >= dma_maps_.size()) {
        return;
    }
    const uint32_t width = frame.width();
    const uint32_t height = frame.height();
    const uint32_t stride = frame.stride_y();
    if(width == 0 || height == 0 || stride == 0) {
        return;
    }
    const uint8_t* src = dma_maps_[idx];
    if(!src) {
        return;
    }
    const uint8_t* src_y = src;
    const uint8_t* src_uv = src + static_cast<size_t>(stride) * height;
    const size_t need = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u / 2u;
    std::lock_guard<std::mutex> lock(frame_mtx_);
    latest_nv12_snapshot_.resize(need);
    copy_nv12_tight(latest_nv12_snapshot_.data(), src_y, src_uv, stride, width, height);
    latest_width_ = width;
    latest_height_ = height;
    latest_frame_seq_ = frame.seq;
    latest_nv12_valid_ = true;
    if(recent_snapshots_.empty() ||
       static_cast<uint32_t>(frame.seq - last_temporal_frame_seq_) >=
           kTemporalFrameStride) {
        FrameSnapshot snapshot;
        snapshot.nv12 = latest_nv12_snapshot_;
        snapshot.width = width;
        snapshot.height = height;
        snapshot.frame_seq = frame.seq;
        recent_snapshots_.push_back(std::move(snapshot));
        while(recent_snapshots_.size() > kTemporalFrameCount) {
            recent_snapshots_.pop_front();
        }
        last_temporal_frame_seq_ = frame.seq;
    }
}

void VisualService::clear_latest_snapshot()
{
    std::lock_guard<std::mutex> lock(frame_mtx_);
    latest_nv12_snapshot_.clear();
    latest_width_ = 0;
    latest_height_ = 0;
    latest_frame_seq_ = 0;
    latest_nv12_valid_ = false;
    recent_snapshots_.clear();
    last_temporal_frame_seq_ = 0;
}

bool VisualService::copy_latest_snapshot(std::vector<uint8_t>& out, uint32_t& width,
                                         uint32_t& height, uint32_t& frame_seq)
{
    std::lock_guard<std::mutex> lock(frame_mtx_);
    if(!latest_nv12_valid_ || latest_nv12_snapshot_.empty()) {
        return false;
    }
    out = latest_nv12_snapshot_;
    width = latest_width_;
    height = latest_height_;
    frame_seq = latest_frame_seq_;
    return true;
}

bool VisualService::copy_recent_snapshots(std::vector<FrameSnapshot>& out)
{
    std::lock_guard<std::mutex> lock(frame_mtx_);
    if(!latest_nv12_valid_ || latest_nv12_snapshot_.empty()) {
        return false;
    }
    out.assign(recent_snapshots_.begin(), recent_snapshots_.end());
    if(out.empty() || out.back().frame_seq != latest_frame_seq_) {
        FrameSnapshot latest;
        latest.nv12 = latest_nv12_snapshot_;
        latest.width = latest_width_;
        latest.height = latest_height_;
        latest.frame_seq = latest_frame_seq_;
        out.push_back(std::move(latest));
    }
    if(out.size() > kTemporalFrameCount) {
        out.erase(out.begin(), out.end() - kTemporalFrameCount);
    }
    return true;
}

bool VisualService::process_first_layer_frames(
    const std::vector<FrameSnapshot>& frames)
{
    uint64_t session_id = 0;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        session_id = first_layer_session_id_;
    }

    if(frames.empty()) {
        UPC_LOG_VISUAL_SERVICE(Warn,
            "first layer inference report_v=2 session_id=%llu status=no_snapshot "
            "frame=0 width=0 height=0 result=0 rejection=no_snapshot",
            static_cast<unsigned long long>(session_id));
        return false;
    }

    const FrameSnapshot& latest = frames.back();
    const std::vector<uint8_t>& nv12 = latest.nv12;
    const uint32_t width = latest.width;
    const uint32_t height = latest.height;
    const uint32_t frame_seq = latest.frame_seq;
    const size_t y_bytes = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t expected_nv12_bytes = y_bytes * 3u / 2u;
    if(width == 0 || height == 0 || nv12.size() != expected_nv12_bytes) {
        UPC_LOG_VISUAL_SERVICE(Error,
            "first layer inference report_v=2 session_id=%llu status=invalid_frame "
            "frame=%u width=%u height=%u size=%zu expected=%zu result=0 "
            "rejection=invalid_nv12",
            static_cast<unsigned long long>(session_id),
            static_cast<unsigned>(frame_seq), static_cast<unsigned>(width),
            static_cast<unsigned>(height), nv12.size(), expected_nv12_bytes);
        return false;
    }

    std::unique_lock<std::mutex> detector_lock(detector_mtx_);
    const FirstLayerDetection latest_detection = first_layer_detector_.predict_nv12(
        nv12.data(), nv12.size(), width, height, frame_seq);
    if(!latest_detection.error.empty()) {
        UPC_LOG_VISUAL_SERVICE(Error,
            "first layer inference report_v=2 session_id=%llu status=inference_error "
            "frame=%u width=%u height=%u total_ms=%.3f result=0 "
            "rejection=inference_error error=%s",
            static_cast<unsigned long long>(session_id),
            static_cast<unsigned>(frame_seq), static_cast<unsigned>(width),
            static_cast<unsigned>(height), latest_detection.total_ms,
            latest_detection.error.c_str());
        return false;
    }

    std::string decision_mode = "direct_reject";
    std::string final_rejection;
    bool final_pass = false;
    double decision_probability = latest_detection.ready_probability;
    std::size_t temporal_usable =
        temporal_detection_usable(latest_detection) ? 1u : 0u;
    std::vector<FirstLayerDetection> detections;
    detections.reserve(frames.size());

    if(latest_detection.ready_probability >= kTemporalDirectAcceptProbability) {
        decision_mode = "direct_accept";
        final_pass = latest_detection.passed;
        if(!final_pass) {
            final_rejection = latest_detection.rejection_reasons.empty()
                ? "latest_frame_rejected" : latest_detection.rejection_reasons;
        }
    } else if(latest_detection.ready_probability < kTemporalDirectRejectProbability) {
        decision_mode = "direct_reject";
        final_rejection = latest_detection.rejection_reasons.empty()
            ? "direct_low_probability" : latest_detection.rejection_reasons;
    } else {
        decision_mode = "temporal_median";
        if(frames.size() != kTemporalFrameCount ||
           !temporal_detection_usable(latest_detection)) {
            final_rejection = frames.size() != kTemporalFrameCount
                ? "temporal_frames_unavailable" : "latest_frame_rejected";
        } else {
            detections.resize(frames.size());
            detections.back() = latest_detection;
            std::vector<double> probabilities;
            probabilities.reserve(frames.size());
            probabilities.push_back(latest_detection.ready_probability);
            temporal_usable = 1;
            bool temporal_error = false;
            for(std::size_t index = 0; index + 1 < frames.size(); ++index) {
                const FrameSnapshot& frame = frames[index];
                const std::size_t frame_y_bytes =
                    static_cast<std::size_t>(frame.width) * frame.height;
                const std::size_t expected = frame_y_bytes * 3u / 2u;
                if(frame.width == 0 || frame.height == 0 ||
                   frame.nv12.size() != expected) {
                    temporal_error = true;
                    break;
                }
                detections[index] = first_layer_detector_.predict_nv12(
                    frame.nv12.data(), frame.nv12.size(), frame.width,
                    frame.height, frame.frame_seq);
                if(!temporal_detection_usable(detections[index])) {
                    temporal_error = true;
                    break;
                }
                ++temporal_usable;
                probabilities.push_back(detections[index].ready_probability);
            }
            if(temporal_error || temporal_usable != kTemporalFrameCount) {
                final_rejection = "temporal_frame_rejected";
            } else {
                std::sort(probabilities.begin(), probabilities.end());
                decision_probability = probabilities[probabilities.size() / 2u];
                final_pass = decision_probability >= fusion_model::kReadyThreshold;
                if(!final_pass) {
                    final_rejection = "temporal_median_below_threshold";
                }
            }
        }
    }

    detector_lock.unlock();

    DiagnosticImageSaveResult qualified_image;
    if(final_pass && latest_detection.passed) {
        qualified_image = save_diagnostic(
            nv12, width, height, frame_seq, latest_detection,
            qualified_image_dir(), "qualified");
    } else if(final_pass && !detections.empty()) {
        for(std::size_t index = detections.size(); index-- > 0;) {
            if(detections[index].passed) {
                const FrameSnapshot& frame = frames[index];
                qualified_image = save_diagnostic(
                    frame.nv12, frame.width, frame.height, frame.frame_seq,
                    detections[index], qualified_image_dir(), "qualified");
                break;
            }
        }
    }

    if(final_pass) {
        std::lock_guard<std::mutex> lock(state_mtx_);
        first_layer_session_passed_ = true;
    } else {
        cache_final_rejected_diagnostic(
            nv12, width, height, frame_seq, latest_detection);
    }

    UPC_LOG_VISUAL_SERVICE(Info,
        "first layer inference report_v=2 session_id=%llu status=ok frame=%u "
        "width=%u height=%u raw_class=%s output_class=%s "
        "ready_probability=%.6f latest_ready_probability=%.6f "
        "decision_mode=%s temporal_frames=%zu temporal_usable=%zu "
        "ready_threshold=%.6f confidence=%.6f confidence_threshold=%.6f "
        "ood=%d ood_distance=%.6f ood_threshold=%.6f alignment=%d cache=%d "
        "cache_event=%s segmentation_ran=%d segmentation_ms=%.3f "
        "preprocessing_ms=%.3f fusion_ms=%.3f total_ms=%.3f "
        "rejection=%s result=%d qualified_image_saved=%d "
        "qualified_image_path=%s image_save_ms=%.3f image_error=%s",
        static_cast<unsigned long long>(session_id),
        static_cast<unsigned>(frame_seq), static_cast<unsigned>(width),
        static_cast<unsigned>(height), latest_detection.raw_class.c_str(),
        latest_detection.output_class.c_str(), decision_probability,
        latest_detection.ready_probability, decision_mode.c_str(), frames.size(),
        temporal_usable, static_cast<double>(fusion_model::kReadyThreshold),
        latest_detection.confidence,
        static_cast<double>(fusion_model::kConfidenceThreshold),
        latest_detection.is_ood ? 1 : 0, latest_detection.ood_distance,
        static_cast<double>(fusion_model::kOodThreshold),
        latest_detection.alignment_passed ? 1 : 0,
        latest_detection.cache_available ? 1 : 0,
        latest_detection.cache_event.c_str(),
        latest_detection.segmentation_ran ? 1 : 0,
        latest_detection.segmentation_ms,
        latest_detection.preprocessing_ms, latest_detection.fusion_ms,
        latest_detection.total_ms, final_rejection.c_str(), final_pass ? 1 : 0,
        qualified_image.saved ? 1 : 0, qualified_image.path.c_str(),
        qualified_image.elapsed_ms, qualified_image.error.c_str());
    if(final_pass && !qualified_image.saved) {
        UPC_LOG_VISUAL_SERVICE(Warn,
            "first layer qualified image save failed frame=%u error=%s",
            static_cast<unsigned>(frame_seq), qualified_image.error.c_str());
    }
    return final_pass;
}
void VisualService::on_first_layer_check_req(const middleware::Message& msg)
{
    middleware::VisualFirstLayerCheckReqPayload req{};
    bool has_req = false;
    if(msg.payload.type == middleware::MsgPayload::Type::Inline &&
       msg.payload.length >= sizeof(req)) {
        std::memcpy(&req, msg.payload.data.bytes.data(), sizeof(req));
        has_req = true;
    }

    uint64_t session_id = 0;
    if(has_req) {
        bool session_active = false;
        {
            std::lock_guard<std::mutex> lock(state_mtx_);
            session_active = first_layer_session_active_;
            session_id = first_layer_session_id_;
        }
        if(!session_active) {
            /* 启动通知丢失时先起流；本次无帧仍按保守策略返回 0。 */
            start_first_layer_session("check_without_start");
            std::lock_guard<std::mutex> lock(state_mtx_);
            session_id = first_layer_session_id_;
        }
    }

    std::vector<FrameSnapshot> frames;
    bool pass = false;
    if(!has_req) {
        UPC_LOG_VISUAL_SERVICE(Warn, "first layer check invalid request payload");
        UPC_LOG_VISUAL_SERVICE(Warn,
            "first layer inference report_v=2 session_id=%llu status=invalid_request frame=0 "
            "width=0 height=0 result=0 rejection=invalid_request",
            static_cast<unsigned long long>(session_id));
    } else if(copy_recent_snapshots(frames)) {
        pass = process_first_layer_frames(frames);
    } else {
        UPC_LOG_VISUAL_SERVICE(Warn, "first layer check no snapshot available");
        UPC_LOG_VISUAL_SERVICE(Warn,
            "first layer inference report_v=2 session_id=%llu status=no_snapshot frame=0 "
            "width=0 height=0 result=0 rejection=no_snapshot",
            static_cast<unsigned long long>(session_id));
    }

    middleware::VisualFirstLayerCheckRspPayload rsp{};
    rsp.result = pass ? 1 : 0;
    middleware::Message out(
        middleware::MsgType::APP_Visual_FirstLayer_Check_Rsp,
        middleware::ModuleId::APP_VISUAL,
        middleware::ModuleId::APP_GLAVO);
    out.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&rsp), sizeof(rsp));
    middleware::MsgPort::instance().publish_async(out);

    UPC_LOG_VISUAL_SERVICE(Info,
        "first layer check done req_valid=%d layer_um=%u result=%u",
        has_req ? 1 : 0,
        has_req ? static_cast<unsigned>(req.layer_height_um) : 0u,
        static_cast<unsigned>(rsp.result));

    if(rsp.result != 0) {
        stop_first_layer_session("first_layer_passed");
    }
}

uint64_t VisualService::monotonic_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace upc::visual
