#include "camera_service.hpp"

#include "device_config.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "msg_port.hpp"
#include "msg_runtime.hpp"
#include "rs_log.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <sys/stat.h>
#include <unistd.h>

namespace upc::camera {

namespace {

constexpr const char* kUiShmName = CAMERA_SHM_NAME_DEFAULT;
constexpr const char* kVisualShmName = "/upc_camera_preview_visual";
constexpr const char* kMediaShmName = "/upc_camera_preview_media";

/**
 * 在飞窗口耗尽时，等待落后消费者归还槽位的上限（帧周期个数）。
 *
 * 这个等待只为「让消费者用完当前帧、再复用底层 buffer」争取时间；超时后无论
 * 归还与否都会把 buffer 还回去（见 stream_thread_fn 窗口收尾处的注释）。所以
 * 上限只需覆盖消费者正常处理一帧的耗时——UI 侧实测单帧 RGA 转换约 7ms、解码
 * 线程整圈约 5ms，两个帧周期（@30fps 约 66ms）已是十倍余量。
 *
 * **不要再改回固定的 1000ms**：那样任何一个消费者卡住，采集线程都会每帧白等
 * 整整 1 秒，把整条采集链路（含直播编码与打印逐层快照）一起拖到 1fps，并且不
 * 会自行恢复——曾据此在现场复现出连续 48 分钟、间隔精确 1.037 秒的预览塌陷。
 * 按帧率推导后，最坏退化被限制成「该消费者掉几帧」，不再扩散到其它消费者。
 */
constexpr uint32_t kPeerWaitSlotEmptyFramePeriods = 2u;

/** 帧率异常（0 或极高）时的等待下限，避免退化成忙等。 */
constexpr int kPeerWaitSlotEmptyMinMs = 20;

/** 按当前帧率算出槽位归还等待上限；fps 为 0 时退到下限。 */
int peer_wait_slot_empty_ms(uint32_t fps)
{
    if(fps == 0u) {
        return kPeerWaitSlotEmptyMinMs;
    }
    const int budget =
        static_cast<int>((1000u * kPeerWaitSlotEmptyFramePeriods) / fps);
    return budget > kPeerWaitSlotEmptyMinMs ? budget : kPeerWaitSlotEmptyMinMs;
}

uint64_t monotonic_us()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now().time_since_epoch())
            .count());
}

const char* env_or_default(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : fallback;
}

} // namespace

CameraService::~CameraService()
{
    shutdown();
}

bool CameraService::init()
{
    stream_fps_            = read_fps_from_env();
    stream_warmup_frames_  = read_warmup_frames_from_env();

    const char* ntf_env = std::getenv(CAMERA_ENV_FRAME_NTF);
    if(ntf_env && ntf_env[0]) {
        frame_ntf_enabled_ = (ntf_env[0] != '0');
    }

    subscribe_messages();
    UPC_LOG_CAMERA_SERVICE(Info,
        "init device=%s %ux%u@%u warmup_frames=%u iq=%s/%s shm=%s frame_ntf=%d",
        env_or_default(CAMERA_ENV_DEVICE, CAMERA_V4L2_DEVICE_DEFAULT),
        stream_width_, stream_height_, stream_fps_, stream_warmup_frames_,
        env_or_default(CAMERA_ENV_IQ_DIR, CAMERA_IQ_DIR_DEFAULT),
        env_or_default(CAMERA_ENV_IQ_FILE, CAMERA_IQ_FILE_DEFAULT),
        CAMERA_SHM_NAME_DEFAULT, frame_ntf_enabled_ ? 1 : 0);
    return true;
}

void CameraService::shutdown()
{
    stop_streaming();
    subs_.clear();
}

ViCaptureConfig CameraService::build_capture_config()
{
    ViCaptureConfig cfg;
    cfg.video_dev = env_or_default(CAMERA_ENV_DEVICE, CAMERA_V4L2_DEVICE_DEFAULT);
    cfg.iq_dir    = env_or_default(CAMERA_ENV_IQ_DIR, CAMERA_IQ_DIR_DEFAULT);
    cfg.iq_file   = env_or_default(CAMERA_ENV_IQ_FILE, CAMERA_IQ_FILE_DEFAULT);
    cfg.width     = stream_width_;
    cfg.height    = stream_height_;
    cfg.fps       = stream_fps_;
    cfg.buf_count = CAMERA_V4L2_BUF_COUNT;
    return cfg;
}

uint32_t CameraService::read_fps_from_env()
{
    const char* env = std::getenv(CAMERA_ENV_FPS);
    if(!env || !env[0]) {
        return CAMERA_FPS_DEFAULT;
    }
    const long v = std::strtol(env, nullptr, 10);
    if(v <= 0 || v > 120) {
        return CAMERA_FPS_DEFAULT;
    }
    return static_cast<uint32_t>(v);
}

uint32_t CameraService::read_warmup_frames_from_env()
{
    const char* env = std::getenv(CAMERA_ENV_WARMUP_FRAMES);
    if(!env || !env[0]) {
        return CAMERA_WARMUP_FRAMES_DEFAULT;
    }
    const long v = std::strtol(env, nullptr, 10);
    if(v < 0 || v > 120) {
        return CAMERA_WARMUP_FRAMES_DEFAULT;
    }
    return static_cast<uint32_t>(v);
}

void CameraService::subscribe_messages()
{
    auto& port = middleware::MsgPort::instance();

    subs_.push_back(port.subscribe(
        middleware::MsgType::UI_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::UI_Camera_Stream_Stop_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::VISUAL_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::VISUAL_Camera_Stream_Stop_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

    subs_.push_back(port.subscribe(
        middleware::MsgType::MEDIA_Camera_Stream_Start_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));
    subs_.push_back(port.subscribe(
        middleware::MsgType::MEDIA_Camera_Stream_Stop_Req,
        middleware::ModuleId::APP_CAMERA,
        [this](const middleware::Message& msg) { handle_message(msg); }));

}

void CameraService::handle_message(const middleware::Message& msg)
{
    switch(msg.type) {
    case middleware::MsgType::UI_Camera_Stream_Start_Req:
        handle_stream_start(msg);
        break;
    case middleware::MsgType::UI_Camera_Stream_Stop_Req:
        handle_stream_stop(msg);
        break;
    case middleware::MsgType::VISUAL_Camera_Stream_Start_Req:
        handle_visual_stream_start(msg);
        break;
    case middleware::MsgType::VISUAL_Camera_Stream_Stop_Req:
        handle_visual_stream_stop(msg);
        break;
    case middleware::MsgType::MEDIA_Camera_Stream_Start_Req:
        handle_media_stream_start(msg);
        break;
    case middleware::MsgType::MEDIA_Camera_Stream_Stop_Req:
        handle_media_stream_stop(msg);
        break;
    default:
        break;
    }
}

void CameraService::handle_stream_start(const middleware::Message& msg)
{
    if(!stream_running_.load()) {
        const bool ok = start_streaming();
        if(!ok) {
            send_stream_start_rsp(msg, false);
            UPC_LOG_CAMERA_SERVICE(Error, "stream start failed");
            return;
        }
        stop_stream_.store(false);
        stream_running_.store(true);
        stream_thread_ = std::thread(&CameraService::stream_thread_fn, this);

        UPC_LOG_CAMERA_SERVICE(Info,
            "streaming live %ux%u @ %u fps stride_y=%u isolation=1 "
            "cap_stride_aligned=%d",
            stream_width_, stream_height_, stream_fps_,
            dma_source_.dma_stride_y(),
            dma_source_.capture_stride_aligned() ? 1 : 0);
    }

    const bool ok = ensure_stream_peer(
        msg, kUiShmName, middleware::MsgType::APP_Camera_Stream_Start_Rsp);
    if(!ok) {
        send_stream_start_rsp(msg, false);
    } else {
        send_stream_start_rsp(msg, true);
    }
}

void CameraService::handle_visual_stream_start(const middleware::Message& msg)
{
    if(!stream_running_.load()) {
        const bool ok = start_streaming();
        if(!ok) {
            send_stream_start_rsp_with_type(
                msg, false, middleware::MsgType::APP_Visual_Camera_Stream_Start_Rsp,
                kVisualShmName);
            UPC_LOG_CAMERA_SERVICE(Error, "visual stream start failed");
            return;
        }
        stop_stream_.store(false);
        stream_running_.store(true);
        stream_thread_ = std::thread(&CameraService::stream_thread_fn, this);

        UPC_LOG_CAMERA_SERVICE(Info,
            "streaming live %ux%u @ %u fps stride_y=%u isolation=1 "
            "cap_stride_aligned=%d",
            stream_width_, stream_height_, stream_fps_,
            dma_source_.dma_stride_y(),
            dma_source_.capture_stride_aligned() ? 1 : 0);
    }

    const bool ok = ensure_stream_peer(
        msg, kVisualShmName, middleware::MsgType::APP_Visual_Camera_Stream_Start_Rsp);
    send_stream_start_rsp_with_type(
        msg, ok, middleware::MsgType::APP_Visual_Camera_Stream_Start_Rsp,
        kVisualShmName);
}

void CameraService::handle_stream_stop(const middleware::Message& msg)
{
    remove_stream_peer(msg.sender_id);
    if(!has_stream_peers()) {
        stop_streaming();
    }
    send_stream_stop_done(msg);
    UPC_LOG_CAMERA_SERVICE(Info, "stream stop ui sender=%u",
                           static_cast<unsigned>(msg.sender_id));
}

void CameraService::handle_visual_stream_stop(const middleware::Message& msg)
{
    remove_stream_peer(msg.sender_id);
    if(!has_stream_peers()) {
        stop_streaming();
    }
    send_stream_stop_done_with_type(
        msg, middleware::MsgType::APP_Visual_Camera_Stream_Stop_Rsp);
    UPC_LOG_CAMERA_SERVICE(Info, "stream stop visual sender=%u",
                           static_cast<unsigned>(msg.sender_id));
}

void CameraService::handle_media_stream_start(const middleware::Message& msg)
{
    if(!stream_running_.load()) {
        const bool ok = start_streaming();
        if(!ok) {
            send_stream_start_rsp_with_type(
                msg, false, middleware::MsgType::APP_Media_Camera_Stream_Start_Rsp,
                kMediaShmName);
            UPC_LOG_CAMERA_SERVICE(Error, "media stream start failed");
            return;
        }
        stop_stream_.store(false);
        stream_running_.store(true);
        stream_thread_ = std::thread(&CameraService::stream_thread_fn, this);

        UPC_LOG_CAMERA_SERVICE(Info,
            "streaming live %ux%u @ %u fps stride_y=%u isolation=1 "
            "cap_stride_aligned=%d",
            stream_width_, stream_height_, stream_fps_,
            dma_source_.dma_stride_y(),
            dma_source_.capture_stride_aligned() ? 1 : 0);
    }

    const bool ok = ensure_stream_peer(
        msg, kMediaShmName, middleware::MsgType::APP_Media_Camera_Stream_Start_Rsp);
    send_stream_start_rsp_with_type(
        msg, ok, middleware::MsgType::APP_Media_Camera_Stream_Start_Rsp,
        kMediaShmName);
}

void CameraService::handle_media_stream_stop(const middleware::Message& msg)
{
    remove_stream_peer(msg.sender_id);
    if(!has_stream_peers()) {
        stop_streaming();
    }
    send_stream_stop_done_with_type(
        msg, middleware::MsgType::APP_Media_Camera_Stream_Stop_Rsp);
    UPC_LOG_CAMERA_SERVICE(Info, "stream stop media sender=%u",
                           static_cast<unsigned>(msg.sender_id));
}

bool CameraService::start_streaming()
{
    const ViCaptureConfig cfg = build_capture_config();
    if(!capture_.open(cfg)) {
        UPC_LOG_CAMERA_SERVICE(Error, "ViCapture open failed");
        return false;
    }

    stream_width_  = capture_.width();
    stream_height_ = capture_.height();

    const std::vector<int>& capture_fds = capture_.dma_fds();
    if(capture_fds.empty()) {
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "no V4L2 dma fds exported");
        return false;
    }

    if(!dma_source_.prepare(stream_width_, stream_height_, capture_.stride_y(),
                            capture_fds)) {
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "dma frame source prepare failed");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(stream_peers_mtx_);
        stream_peers_.clear();
    }

    return true;
}

void CameraService::stop_streaming()
{
    stop_stream_.store(true);
    if(stream_thread_.joinable()) {
        stream_thread_.join();
    }
    stream_running_.store(false);
    {
        std::lock_guard<std::mutex> lock(stream_peers_mtx_);
        for(auto& kv : stream_peers_) {
            if(kv.second) {
                kv.second->producer.destroy();
            }
        }
        stream_peers_.clear();
    }
    dma_source_.shutdown();
    capture_.close();
}

bool CameraService::ensure_stream_peer(const middleware::Message& req,
                                       const char* shm_name,
                                       middleware::MsgType rsp_type)
{
    if(!shm_name || !shm_name[0]) {
        return false;
    }
    const uint32_t key = static_cast<uint32_t>(req.sender_id);
    std::lock_guard<std::mutex> lock(stream_peers_mtx_);
    auto it = stream_peers_.find(key);
    if(it != stream_peers_.end() && it->second) {
        return true;
    }

    auto peer = std::make_shared<StreamPeerSession>();
    peer->peer_id = req.sender_id;
    peer->rsp_type = rsp_type;
    peer->shm_name = shm_name;
    const upc_ipc::IpcShmError err = peer->producer.create(
        peer->shm_name.c_str(),
        stream_width_,
        stream_height_,
        dma_source_.dma_stride_y(),
        upc_ipc::kDmaFrameFormatNv12,
        CAMERA_SHM_SLOT_COUNT);
    if(err != upc_ipc::IpcShmError::OK) {
        UPC_LOG_CAMERA_SERVICE(Error,
            "peer shm create fail type=%u sender=%u name=%s err=%d",
            static_cast<unsigned>(rsp_type),
            key,
            peer->shm_name.c_str(),
            static_cast<int>(err));
        return false;
    }
    stream_peers_[key] = peer;
    UPC_LOG_CAMERA_SERVICE(Info,
        "peer shm ready sender=%u name=%s %ux%u stride_y=%u slots=%u",
        key, peer->shm_name.c_str(), stream_width_, stream_height_,
        dma_source_.dma_stride_y(), CAMERA_SHM_SLOT_COUNT);
    return true;
}

void CameraService::remove_stream_peer(middleware::ModuleId peer)
{
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(stream_peers_mtx_);
        const uint32_t key = static_cast<uint32_t>(peer);
        auto it = stream_peers_.find(key);
        if(it != stream_peers_.end()) {
            if(it->second) {
                /* 采集线程的在飞队列可能还持有本会话的槽位，先解除等待。 */
                it->second->active.store(false, std::memory_order_release);
            }
            stream_peers_.erase(it);
            removed = true;
        }
    }
    if(removed) {
        UPC_LOG_CAMERA_SERVICE(Info, "peer shm removed sender=%u",
                               static_cast<unsigned>(peer));
    }
}

bool CameraService::has_stream_peers() const
{
    std::lock_guard<std::mutex> lock(stream_peers_mtx_);
    return !stream_peers_.empty();
}

void CameraService::stream_thread_fn()
{
    const auto frame_period =
        std::chrono::microseconds(1000000u / (stream_fps_ > 0 ? stream_fps_ : 1u));

    using clock = std::chrono::steady_clock;
    auto stats_since   = clock::now();
    uint32_t frames_1s       = 0;
    uint32_t last_seq        = 0;
    uint32_t last_slot       = 0;
    uint32_t slot_fail_1s    = 0;
    uint32_t dequeue_fail_1s = 0;
    uint32_t timeout_1s      = 0;
    uint32_t inflight_max_1s = 0;
    bool logged_first_commit = false;
    uint32_t warmup_left = stream_warmup_frames_;
    uint64_t last_frame_ok_ms = monotonic_ms();
    uint64_t last_watchdog_restart_ms = 0;

    /** 本帧在某个 peer 环上占用的槽位。 */
    struct PeerCommit {
        std::shared_ptr<StreamPeerSession> peer;
        uint32_t slot_idx;
        uint32_t seq;
    };
    /** 已 commit 给消费者、底层 buffer 尚未归还的一帧。 */
    struct InFlightFrame {
        uint32_t buf_idx;                /**< 待归还的采集 / padding buffer */
        std::vector<PeerCommit> commits; /**< 各 peer 上占用的槽位 */
    };
    std::deque<InFlightFrame> in_flight;

    /* 归还隔离 pool 槽；SHM 槽位由 Consumer 侧 release_slot 恢复。
     * V4L2 buf 在拷贝后已立刻 QBUF，不进在飞队列。 */
    auto release_frame_buffer = [this](uint32_t idx) {
        dma_source_.release_padded(idx);
    };

    /*
     * 在飞窗口上限：隔离 pool 留 2 个给采集线程持续拷贝，SHM 留 1 个给
     * 下一次 acquire_write_slot，再与配置上限取小。看门狗重启后要重算。
     */
    auto compute_max_in_flight = [this]() -> size_t {
        const uint32_t bufs = dma_source_.buffer_count();
        const uint32_t buf_budget  = bufs > 2u ? bufs - 2u : 1u;
        const uint32_t slot_budget = CAMERA_SHM_SLOT_COUNT > 1u
                                         ? CAMERA_SHM_SLOT_COUNT - 1u
                                         : 1u;
        uint32_t n = buf_budget < slot_budget ? buf_budget : slot_budget;
        if(n > CAMERA_MAX_FRAMES_IN_FLIGHT) {
            n = CAMERA_MAX_FRAMES_IN_FLIGHT;
        }
        return n > 0u ? static_cast<size_t>(n) : 1u;
    };
    size_t max_in_flight = compute_max_in_flight();
    /* 与 max_in_flight 同源：看门狗重启后帧率可能变，两个都要重算。 */
    int peer_wait_ms = peer_wait_slot_empty_ms(stream_fps_);

    /*
     * 非阻塞回收队头已被所有 peer 归还的帧。
     * 消费者取最新帧时会把更旧的 Ready 槽直接标 Empty，队头总是最先完成，
     * 因此 FIFO 扫描即可，不必遍历整个队列。
     */
    auto reclaim_completed = [&]() {
        while(!in_flight.empty()) {
            const InFlightFrame& oldest = in_flight.front();
            bool done = true;
            for(const auto& c : oldest.commits) {
                /* 已停流的 peer 不会再 release_slot，直接判完成。 */
                if(!c.peer->active.load(std::memory_order_acquire)) {
                    continue;
                }
                if(!c.peer->producer.slot_is_empty(c.slot_idx)) {
                    done = false;
                    break;
                }
            }
            if(!done) {
                break;
            }
            release_frame_buffer(oldest.buf_idx);
            in_flight.pop_front();
        }
    };

    /*
     * 等最老在飞帧的各 peer 槽变空（最多 peer_wait_ms）。
     * 超时只打日志，绝不强行 release——对齐 libcamera/GStreamer：
     * 宁可丢新帧，也不把消费者还在读的 buffer 塞回去。
     */
    auto wait_oldest_peers = [&](int wait_ms) {
        if(in_flight.empty()) {
            return;
        }
        const InFlightFrame& oldest = in_flight.front();
        for(const auto& c : oldest.commits) {
            if(!c.peer->active.load(std::memory_order_acquire)) {
                continue;
            }
            if(c.peer->producer.wait_slot_empty(c.slot_idx, wait_ms)) {
                continue;
            }
            ++timeout_1s;
            UPC_LOG_CAMERA_SERVICE(Warn,
                "slot release slow sender=%u slot=%u seq=%u buf_idx=%u "
                "(drop new frames, keep buffer)",
                static_cast<unsigned>(c.peer->peer_id),
                c.slot_idx, c.seq, oldest.buf_idx);
        }
    };

    if(warmup_left > 0) {
        UPC_LOG_CAMERA_SERVICE(Info,
            "AE warmup: discarding first %u frames before SHM commit",
            warmup_left);
    }

    while(!stop_stream_.load(std::memory_order_relaxed)) {
        const auto now = clock::now();
        if(now - stats_since >= std::chrono::seconds(5)) {
            /* 流诊断受 UPC_MOD_CAMERA_PREVIEW 控制，5s 聚合输出 */
            const ViCaptureLatencyStats lat = capture_.take_latency_stats();
            const uint64_t drv_age_avg_us =
                lat.samples > 0 ? (lat.latency_us_sum / lat.samples) : 0;
            UPC_LOG_CAMERA_PREVIEW(Debug,
                "stream 5s: committed=%u last_seq=%u last_slot=%u "
                "slot_fail=%u dequeue_fail=%u release_timeout=%u "
                "inflight_max=%u/%zu "
                "drv_age_avg_us=%llu drv_age_max_us=%llu drv_drop=%u "
                "drv_drained=%u ts_mono=%d",
                frames_1s, last_seq, last_slot,
                slot_fail_1s, dequeue_fail_1s, timeout_1s,
                inflight_max_1s, max_in_flight,
                static_cast<unsigned long long>(drv_age_avg_us),
                static_cast<unsigned long long>(lat.latency_us_max),
                lat.seq_gap, lat.drained,
                lat.timestamp_monotonic ? 1 : 0);
            frames_1s = slot_fail_1s = dequeue_fail_1s = timeout_1s = 0;
            inflight_max_1s = 0;
            stats_since = now;
        }

        const auto tick_start = clock::now();

        /* 每轮开头先收一次：预热与 dequeue 失败分支都会提前 continue，
         * 只有放在这里才能保证 buffer 不滞留在在飞队列。 */
        reclaim_completed();

        auto try_watchdog_restart = [&](const char* why) -> bool {
#if CAMERA_STREAM_WATCHDOG_STALL_MS == 0
            (void)why;
            return false;
#else
            const uint64_t now_ms = monotonic_ms();
            if(now_ms - last_frame_ok_ms < CAMERA_STREAM_WATCHDOG_STALL_MS) {
                return false;
            }
            if(last_watchdog_restart_ms != 0 &&
               now_ms - last_watchdog_restart_ms <
                   CAMERA_STREAM_WATCHDOG_COOLDOWN_MS) {
                return false;
            }
            UPC_LOG_CAMERA_SERVICE(Warn,
                "stream watchdog trigger why=%s stall_ms=%llu",
                why ? why : "?",
                static_cast<unsigned long long>(now_ms - last_frame_ok_ms));
            last_watchdog_restart_ms = now_ms;
            /*
             * 拆 ISP/隔离 pool 前尽量等在飞帧被消费者放完，避免 STREAMOFF /
             * free pool 时 UI 仍在 import/mmap。等不到也不强行 QBUF 在飞 buf：
             * 隔离路径下消费者握的是 heap fd，Camera close 自己的 fd 后
             * 内存仍由消费者 fd 引用撑着，直到它们收到新 Start_Rsp 换 fd。
             */
            {
                const auto drain_deadline =
                    clock::now() + std::chrono::milliseconds(
                        peer_wait_slot_empty_ms(stream_fps_));
                while(!in_flight.empty() && clock::now() < drain_deadline) {
                    reclaim_completed();
                    if(in_flight.empty()) {
                        break;
                    }
                    wait_oldest_peers(peer_wait_ms);
                    reclaim_completed();
                    if(!in_flight.empty()) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(2));
                    }
                }
                if(!in_flight.empty()) {
                    UPC_LOG_CAMERA_SERVICE(Warn,
                        "stream watchdog: abandon %zu in-flight buffers "
                        "before restart (consumers still hold)",
                        in_flight.size());
                    in_flight.clear();
                }
            }
            if(!restart_capture_keep_peers()) {
                last_frame_ok_ms = monotonic_ms();
                return false;
            }
            warmup_left = stream_warmup_frames_;
            logged_first_commit = false;
            last_frame_ok_ms = monotonic_ms();
            max_in_flight = compute_max_in_flight();
            peer_wait_ms = peer_wait_slot_empty_ms(stream_fps_);
            if(warmup_left > 0) {
                UPC_LOG_CAMERA_SERVICE(Info,
                    "stream watchdog: AE warmup %u frames after restart",
                    warmup_left);
            }
            return true;
#endif
        };

        /* AE 预热：只 dequeue/queue 喂 ISP，不占 SHM slot、不 commit。 */
        if(warmup_left > 0) {
            std::optional<uint32_t> buf_idx = capture_.dequeue_frame(2000);
            if(!buf_idx) {
                ++dequeue_fail_1s;
                if(stop_stream_.load()) {
                    break;
                }
                (void)try_watchdog_restart("warmup_dequeue");
                continue;
            }
            last_frame_ok_ms = monotonic_ms();
            capture_.queue_frame(*buf_idx);
            --warmup_left;
            if(warmup_left == 0) {
                UPC_LOG_CAMERA_SERVICE(Info,
                    "AE warmup done, committing frames to SHM");
            }
            const auto elapsed = clock::now() - tick_start;
            if(elapsed < frame_period) {
                std::this_thread::sleep_for(frame_period - elapsed);
            }
            continue;
        }

        std::optional<uint32_t> buf_idx = capture_.dequeue_frame(2000);
        if(!buf_idx) {
            ++dequeue_fail_1s;
            if(stop_stream_.load()) {
                break;
            }
            (void)try_watchdog_restart("dequeue");
            continue;
        }
        last_frame_ok_ms = monotonic_ms();

        /*
         * 始终拷到隔离 pool，V4L2 buf 立刻 QBUF 还给 ISP。
         * 消费者只碰 heap fd，与 rkisp IOMMU 无共享 sg。
         */
        const int padded_idx = dma_source_.acquire_padded();
        if(padded_idx < 0) {
            capture_.queue_frame(*buf_idx);
            ++slot_fail_1s;
            /*
             * pool 耗尽通常是在飞窗口顶满。先给最老帧一次短等，再决定是否
             * 丢掉本帧（绝不把仍被持有的 isolation buf 提前 release）。
             */
            if(in_flight.size() >= max_in_flight) {
                wait_oldest_peers(peer_wait_ms);
                reclaim_completed();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        dma_source_.copy_padded_frame(
            static_cast<uint32_t>(padded_idx),
            capture_.buffer_y(*buf_idx),
            capture_.buffer_uv(*buf_idx),
            capture_.stride_y(),
            stream_width_,
            stream_height_);
        capture_.queue_frame(*buf_idx);
        const uint32_t slot_buf_idx = static_cast<uint32_t>(padded_idx);

        /*
         * 窗口已满：先短等最老帧，仍满则丢弃本帧（还 isolation 槽），
         * 绝不 release 仍被消费者持有的在飞 buffer。
         */
        if(in_flight.size() >= max_in_flight) {
            wait_oldest_peers(peer_wait_ms);
            reclaim_completed();
        }
        if(in_flight.size() >= max_in_flight) {
            ++timeout_1s;
            dma_source_.release_padded(slot_buf_idx);
            continue;
        }

        std::vector<PeerCommit> commits;
        {
            std::vector<std::shared_ptr<StreamPeerSession>> peers;
            {
                std::lock_guard<std::mutex> lock(stream_peers_mtx_);
                peers.reserve(stream_peers_.size());
                for(auto& kv : stream_peers_) {
                    if(kv.second) {
                        peers.push_back(kv.second);
                    }
                }
            }
            if(peers.empty()) {
                ++slot_fail_1s;
            } else {
                for(const auto& peer : peers) {
                    std::optional<uint32_t> slot = peer->producer.acquire_write_slot();
                    if(!slot) {
                        ++slot_fail_1s;
                        continue;
                    }
                    const uint32_t seq =
                        peer->producer.commit_slot(*slot, monotonic_us(), slot_buf_idx);
                    if(seq == 0) {
                        peer->producer.cancel_write_slot(*slot);
                        ++slot_fail_1s;
                        continue;
                    }
                    commits.push_back(PeerCommit{peer, *slot, seq});
                }
            }
        }

        if(commits.empty()) {
            /* 没有 peer 拿到槽：本帧 isolation buf 立刻还池，不进在飞。 */
            dma_source_.release_padded(slot_buf_idx);
            const auto elapsed = clock::now() - tick_start;
            if(elapsed < frame_period) {
                std::this_thread::sleep_for(frame_period - elapsed);
            }
            continue;
        }

        if(!logged_first_commit) {
            UPC_LOG_CAMERA_SERVICE(Info,
                "first frame committed peers=%zu seq=%u buf_idx=%u "
                "isolation=1 cap_stride_aligned=%d",
                commits.size(), commits[0].seq, slot_buf_idx,
                dma_source_.capture_stride_aligned() ? 1 : 0);
            logged_first_commit = true;
        }
        if(frame_ntf_enabled_) {
            notify_frame(commits[0].seq);
        }
        frames_1s++;
        last_seq  = commits[0].seq;
        last_slot = commits[0].slot_idx;

        /* 本帧挂进在飞队列：isolation buf 要等所有 peer 归还槽位后才能回收。 */
        in_flight.push_back(InFlightFrame{slot_buf_idx, std::move(commits)});
        if(in_flight.size() > inflight_max_1s) {
            inflight_max_1s = static_cast<uint32_t>(in_flight.size());
        }

        /* 消费者跟得上时，这一轮就把窗口收干净，队列长期保持在 1。 */
        reclaim_completed();

        /*
         * 按帧周期补睡，避免空转把 CPU 打满。
         * sleep_for 会过冲几十微秒，本线程因此比 sensor 略慢；这点差额过去会
         * 在驱动队列里累积成不断增长的画面延迟，现在由 dequeue_frame 的排空
         * 兜住（见 vi_capture.cpp），所以这里只当限流用。
         */
        const auto elapsed = clock::now() - tick_start;
        if(elapsed < frame_period) {
            std::this_thread::sleep_for(frame_period - elapsed);
        }
    }

    /* 退出前归还仍在飞的 isolation buf，避免 pool 计数残留到下次开流。 */
    for(const auto& f : in_flight) {
        release_frame_buffer(f.buf_idx);
    }
    in_flight.clear();
}

void CameraService::send_stream_start_rsp(const middleware::Message& req, bool ok)
{
    send_stream_start_rsp_with_type(req, ok,
        middleware::MsgType::APP_Camera_Stream_Start_Rsp, kUiShmName);
}

void CameraService::publish_stream_start_to_peer(middleware::ModuleId peer_id,
                                                  middleware::MsgType rsp_type,
                                                  const char* shm_name,
                                                  bool ok)
{
    middleware::CameraStreamInfoPayload info{};
    std::snprintf(info.shm_name, sizeof(info.shm_name), "%s",
                  shm_name ? shm_name : "");
    info.width         = stream_width_;
    info.height        = stream_height_;
    info.stride_y      = dma_source_.dma_stride_y();
    info.format        = upc_ipc::kDmaFrameFormatNv12;
    info.fps           = stream_fps_;
    info.dma_buf_count = ok ? dma_source_.buffer_count() : 0;
    info.dma_mode      = ok ? 1 : 0;
    info.ok            = ok ? 1 : 0;

    middleware::Message rsp(
        rsp_type,
        middleware::ModuleId::APP_CAMERA,
        peer_id);
    rsp.priority = middleware::MsgPriority::High;
    rsp.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&info), sizeof(info));

    auto& port = middleware::MsgPort::instance();
    if(ok && !dma_source_.ipc_fds().empty()) {
        if(!port.publish_to_peer_with_fds(rsp, dma_source_.ipc_fds())) {
            UPC_LOG_CAMERA_SERVICE(Error,
                "stream start rsp with fds failed peer=%u type=%u",
                static_cast<unsigned>(peer_id),
                static_cast<unsigned>(rsp_type));
        }
    } else {
        port.publish_async(rsp);
    }
}

void CameraService::send_stream_start_rsp_with_type(
    const middleware::Message& req, bool ok,
    middleware::MsgType rsp_type, const char* shm_name)
{
    publish_stream_start_to_peer(req.sender_id, rsp_type, shm_name, ok);
}

bool CameraService::restart_capture_keep_peers()
{
    UPC_LOG_CAMERA_SERVICE(Warn,
        "stream watchdog: restarting ISP/V4L2 (keep peers)");

    const uint32_t old_width    = stream_width_;
    const uint32_t old_height   = stream_height_;
    const uint32_t old_stride_y = dma_source_.dma_stride_y();

    dma_source_.shutdown();
    capture_.close();

    if(stop_stream_.load(std::memory_order_relaxed)) {
        return false;
    }

    const ViCaptureConfig cfg = build_capture_config();
    if(!capture_.open(cfg)) {
        UPC_LOG_CAMERA_SERVICE(Error, "stream watchdog: ViCapture reopen failed");
        return false;
    }
    stream_width_  = capture_.width();
    stream_height_ = capture_.height();

    const std::vector<int>& capture_fds = capture_.dma_fds();
    if(capture_fds.empty()) {
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "stream watchdog: no dma fds after reopen");
        return false;
    }
    if(!dma_source_.prepare(stream_width_, stream_height_, capture_.stride_y(),
                            capture_fds)) {
        capture_.close();
        UPC_LOG_CAMERA_SERVICE(Error, "stream watchdog: dma prepare failed");
        return false;
    }
    const uint32_t new_stride_y = dma_source_.dma_stride_y();
    const bool peer_shm_layout_changed =
        (old_width != stream_width_) ||
        (old_height != stream_height_) ||
        (old_stride_y != new_stride_y);

    struct PeerResync {
        middleware::ModuleId peer_id;
        middleware::MsgType rsp_type;
        std::string shm_name;
    };
    std::vector<PeerResync> peers;
    {
        std::lock_guard<std::mutex> lock(stream_peers_mtx_);
        for(auto& kv : stream_peers_) {
            if(!kv.second) {
                continue;
            }
            auto& peer = *kv.second;
            if(peer_shm_layout_changed) {
                peer.producer.destroy();
                const upc_ipc::IpcShmError err = peer.producer.create(
                    peer.shm_name.c_str(),
                    stream_width_,
                    stream_height_,
                    new_stride_y,
                    upc_ipc::kDmaFrameFormatNv12,
                    CAMERA_SHM_SLOT_COUNT);
                if(err != upc_ipc::IpcShmError::OK) {
                    UPC_LOG_CAMERA_SERVICE(Error,
                        "stream watchdog: peer shm recreate fail sender=%u name=%s err=%d",
                        kv.first, peer.shm_name.c_str(), static_cast<int>(err));
                    continue;
                }
            }
            peers.push_back(PeerResync{
                peer.peer_id, peer.rsp_type, peer.shm_name});
        }
    }

    for(const auto& p : peers) {
        publish_stream_start_to_peer(p.peer_id, p.rsp_type, p.shm_name.c_str(),
                                     true);
    }

    UPC_LOG_CAMERA_SERVICE(Info,
        "stream watchdog: capture restarted peers=%zu %ux%u stride_y=%u "
        "peer_shm_recreate=%d",
        peers.size(), stream_width_, stream_height_,
        new_stride_y, peer_shm_layout_changed ? 1 : 0);
    return true;
}

uint64_t CameraService::monotonic_ms()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now().time_since_epoch())
            .count());
}

void CameraService::send_stream_stop_done(const middleware::Message& req)
{
    send_stream_stop_done_with_type(
        req, middleware::MsgType::APP_Camera_Stream_Stop_Rsp);
}

void CameraService::send_stream_stop_done_with_type(
    const middleware::Message& req, middleware::MsgType done_type)
{
    middleware::Message done(
        done_type,
        middleware::ModuleId::APP_CAMERA,
        req.sender_id);
    done.priority = middleware::MsgPriority::High;
    middleware::MsgPort::instance().publish_async(done);
}

void CameraService::notify_frame(uint32_t seq)
{
    middleware::CameraFrameNtfPayload ntf{};
    ntf.seq = seq;

    middleware::Message msg(
        middleware::MsgType::APP_Camera_Frame_Ntf,
        middleware::ModuleId::APP_CAMERA,
        middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&ntf), sizeof(ntf));
    middleware::MsgPort::instance().publish_async(msg);
}

} // namespace upc::camera
