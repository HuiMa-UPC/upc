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
    void update_latest_nv12_snapshot(const upc_ipc::IpcDmaFrameView& frame);
    bool copy_latest_nv12_snapshot(std::vector<uint8_t>& out) const;
    bool save_latest_png_snapshot(std::string& out_path) const;
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
    static void copy_nv12_tight(uint8_t* dst, const uint8_t* src_y,
                                const uint8_t* src_uv, uint32_t src_stride_y,
                                uint32_t width, uint32_t height);

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

    mutable std::mutex latest_nv12_mtx_;
    std::vector<uint8_t> latest_nv12_snapshot_;
    bool latest_nv12_valid_ = false;
    uint32_t stream_width_ = 0;
    uint32_t stream_height_ = 0;
    uint32_t stream_stride_y_ = 0;
    uint32_t stream_fps_ = 0;

    mutable std::mutex finalize_mtx_;
    mutable std::mutex storage_mtx_;
    bool clean_video_active_ = false;
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
};

} // namespace upc::media

#endif
