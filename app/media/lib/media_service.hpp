#ifndef UPC_MEDIA_SERVICE_HPP
#define UPC_MEDIA_SERVICE_HPP

#include "clean_video_recorder.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "live_rtsp_pusher.hpp"
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
    void handle_storage_usage_req(const middleware::Message& msg) const;
    void handle_rtsp_start_req(const middleware::Message& msg);
    void handle_rtsp_stop_req(const middleware::Message& msg);
    void send_rtsp_rsp(middleware::MsgType type,
                       const middleware::RtspPushRspPayload& rsp) const;
    /** 开机/开打前：尽力收尾 pending，再清空 tmp 与遗留 __work_* */
    void recover_pending_and_reset_tmp();
    void remove_legacy_work_dirs() const;
    void clear_video_tmp_dir() const;
    void drop_orphan_pending_flags() const;
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
    bool clean_video_active_ = false;
    bool finalize_pending_ = false;
    std::string pending_finalize_frames_dir_;
    bool latest_print_session_closed_ = false;

    PrintVideoRecorder print_video_;
    PrintH264Recorder print_h264_;
    CleanVideoRecorder clean_video_;
    LiveRtspPusher live_rtsp_;
};

} // namespace upc::media

#endif
