#ifndef UPC_MEDIA_SERVICE_HPP
#define UPC_MEDIA_SERVICE_HPP

#include "clean_video_recorder.hpp"
#include "h264_stream_encoder.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "live_rtsp_pusher.hpp"
#include "local_rtsp_server.hpp"
#include "msg_port.hpp"
#include "print_h264_recorder.hpp"
#include "print_video_recorder.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace upc::media {

class MediaService {
public:
    MediaService() = default;
    ~MediaService();

    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;

    bool init();
    void shutdown();
    void tick();

private:
    void subscribe_messages();
    void handle_message(const middleware::Message& msg);
    void handle_snapshot_req(const middleware::Message& msg);
    void handle_print_video(const middleware::Message& msg);
    void handle_clean_video(const middleware::Message& msg);
    void handle_print_video_finalize(const middleware::Message& msg);
    /** 空闲清粉：连续 H264（不受 Print H264 Rec 影响）。 */
    void start_clean_h264(const middleware::CleanVideoSessionStartPayload& req);
    void stop_clean_h264(const middleware::CleanVideoSessionStopPayload& req);
    void complete_clean_h264(uint8_t result_code);
    void publish_clean_test_done_ntf(uint8_t result_code) const;
    /** H264 worker 关闭裸流后触发，解开开门最终化栅栏。 */
    void on_print_h264_finalize_done();
    /** H264 录制因空间耗尽提前收尾，通知 print_video 取消 print_h264 期待位。 */
    void on_print_h264_storage_exhausted();
    /** Timelapse 优先 finalize；若 H264 仍在收尾则延后 print_h264 remux。 */
    void finalize_print_video_by_stage(const std::string& frames_dir);
    /** 第二阶段：仅完成打印录像 remux 并清 print_video_pending。 */
    void finalize_print_video_stage2(const std::string& frames_dir);
    /** 在 finalize_mtx_ 保护下取出满足全部条件的最终化目录。 */
    std::string take_ready_finalize_locked();
    void handle_storage_usage_req(const middleware::Message& msg) const;
    void handle_rtsp_start_req(const middleware::Message& msg);
    void handle_rtsp_stop_req(const middleware::Message& msg);
    /** 调试页开关：开/停本地局域网 RTSP Server，随后回报实际状态。 */
    void handle_local_rtsp_set(const middleware::Message& msg);
    void handle_local_rtsp_get(const middleware::Message& msg);
    /**
     * 按 local_rtsp_desired_ 拉起本地 RTSP。
     * 相机流就绪后调用：开流前编码几何未知，start() 必失败，故上电默认开启
     * 只能推迟到这里；已在运行则跳过（相机看门狗重启不会打断在连客户端）。
     * IPv4 监听因 TIME_WAIT 失败时由 tick() 限频重试 init()。
     */
    void apply_local_rtsp_desired();
    /** 向 UI 回报本地 RTSP Server 实际开关状态（开启失败时为 0）。 */
    void publish_local_rtsp_state() const;
    void send_rtsp_rsp(middleware::MsgType type,
                       const middleware::RtspPushRspPayload& rsp) const;
    /** 开机/开打前：尽力收尾 pending，再清空 tmp 与遗留 __work_* */
    void recover_pending_and_reset_tmp();
    void remove_legacy_work_dirs() const;
    bool clear_video_tmp_dir() const;
    void drop_orphan_pending_flags() const;
    bool reclaim_orphan_or_stale_pending_for_budget() const;
    bool is_protected_frames_dir(const std::string& frames_dir) const;
    /** 环形录像区回收：删除最旧的已提交完整会话直至满足 80% 配额与系统余量。 */
    bool ensure_video_storage_budget() const;
    /**
     * 逐帧写盘热路径专用：ensure_video_storage_budget() 的节流版本。
     *
     * 全量核算会递归遍历整个录像目录（层拍 JPEG 可达上千个文件），按帧调用时
     * 开销随打印层数线性增长，会把 H264 编码 worker 拖死。这里只缓存"预算充足"
     * 的结论并在 UPC_MEDIA_STORAGE_BUDGET_CACHE_MS 内复用；一旦判定不足就不再
     * 走缓存，每次都重新核算——否则一次瞬时不足会误停整场录像。
     */
    bool ensure_video_storage_budget_throttled() const;
    static uint64_t query_directory_bytes(const char* path);
    bool evict_oldest_committed_video() const;
    /** 结果页退出：先腾空间再 commit；失败则逐个删最旧已提交会话后重试。 */
    bool commit_history_with_space(const char* frames_dir);
    /** 0~100；失败返回 -1 */
    int query_storage_used_pct() const;
    void apply_storage_watermark(middleware::PrintVideoSessionPayload& info);

    void send_stream_start_req();
    void send_stream_stop_req();
    void on_stream_start_rsp(const middleware::Message& msg);
    void on_stream_stop_done(const middleware::Message& msg);
    void stop_worker();
    void worker_thread_fn();

    void close_dma_fds();
    void unmap_dma_views();
    bool map_dma_views_if_needed();
    void publish_latest_nv12_snapshot(uint32_t width, uint32_t height);
    bool copy_latest_nv12_snapshot(std::vector<uint8_t>& out) const;
    bool save_latest_jpeg_snapshot(std::string& out_path) const;
    void send_snapshot_rsp(const middleware::Message& req, bool ok,
                           const char* path) const;

    static bool read_inline_start_rsp(const middleware::Message& msg,
                                      middleware::CameraStreamInfoPayload& out);
    /** Inline 或 BufferRef（>INLINE_SIZE 走 BufferPool）均可 memcpy 为 POD。 */
    template <typename T>
    static bool read_inline_pod(const middleware::MsgPayload& payload, T& out)
    {
        const uint8_t* base = nullptr;
        size_t data_len = 0;
        if(payload.type == middleware::MsgPayload::Type::Inline) {
            base = payload.data.bytes.data();
            data_len = payload.length;
        } else if(payload.type == middleware::MsgPayload::Type::BufferRef) {
            const auto& ref = payload.data.buffer_ref;
            uint8_t* ptr = middleware::BufferPool::instance().get_ptr(ref.buf_id);
            if(!ptr) {
                return false;
            }
            base = ptr + ref.offset;
            data_len = ref.length;
        } else {
            return false;
        }
        if(!base || data_len < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, base, sizeof(T));
        return true;
    }

    static uint64_t monotonic_ms();
    static uint64_t monotonic_us();
    static std::string format_stamp_local();
    static int run_shell_cmd(const char* cmd, const char* log_tag);

    std::vector<middleware::Subscription> subs_;
    upc_ipc::IpcDmaFrameRingConsumer consumer_;
    std::vector<int> dma_fds_;
    std::vector<uint8_t*> dma_maps_;
    size_t dma_map_size_ = 0;

    mutable std::mutex state_mtx_;
    bool start_req_sent_ = false;
    bool stream_up_ = false;
    uint64_t last_start_req_ms_ = 0;
    middleware::CameraStreamInfoPayload stream_info_{};

    std::thread worker_;
    std::atomic<bool> stop_worker_{false};

    /** 取帧线程私有：本帧的紧凑 NV12，持槽期间只填这一份，随后与快照互换。 */
    std::vector<uint8_t> staging_nv12_;

    mutable std::mutex latest_nv12_mtx_;
    std::vector<uint8_t> latest_nv12_snapshot_;
    bool latest_nv12_valid_ = false;
    uint32_t stream_width_ = 0;
    uint32_t stream_height_ = 0;
    uint32_t stream_stride_y_ = 0;
    uint32_t stream_fps_ = 0;

    mutable std::mutex finalize_mtx_;
    mutable std::mutex storage_mtx_;
    /** 预算检查节流缓存；只缓存"充足"结论，见 ensure_video_storage_budget_throttled()。 */
    mutable std::mutex storage_cache_mtx_;
    mutable bool storage_budget_cached_ok_ = false;
    mutable uint64_t storage_budget_checked_ms_ = 0;
    bool clean_video_active_ = false;
    /** 空闲清粉连续 H264 会话（与结果页 JPEG 清粉互斥）。 */
    bool clean_h264_active_ = false;
    uint8_t clean_h264_result_code_ = 0xff;
    char clean_h264_session_id_[32]{};
    bool finalize_pending_ = false;
    std::string pending_finalize_frames_dir_;
    bool h264_finalize_pending_ = false;
    bool latest_print_session_closed_ = false;

    PrintVideoRecorder print_video_;
    PrintH264Recorder print_h264_;
    CleanVideoRecorder clean_video_;
    /* 必须声明在两个消费者之前：析构逆序，它们摘 sink 时编码器须仍存活。 */
    H264StreamEncoder h264_encoder_;
    LiveRtspPusher live_rtsp_;
    LocalRtspServer local_rtsp_;
    /** 本地 RTSP 的期望状态：上电取 UPC_LOCAL_RTSP_AUTO_START，之后跟随调试页开关。
     *  与 active() 分开记，才能在相机流恢复后重建用户真正想要的状态。 */
    bool local_rtsp_desired_ = (UPC_LOCAL_RTSP_AUTO_START != 0);
    uint64_t last_rtsp_listen_retry_ms_ = 0;
};

} // namespace upc::media

#endif
