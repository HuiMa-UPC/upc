#ifndef UPC_MEDIA_SERVICE_HPP
#define UPC_MEDIA_SERVICE_HPP

#include "clean_video_recorder.hpp"
#include "ipc_dma_frame_ring.hpp"
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
    template <typename T>
    static bool read_inline_pod(const middleware::MsgPayload& payload, T& out)
    {
        if(payload.type != middleware::MsgPayload::Type::Inline ||
           payload.length < sizeof(T)) {
            return false;
        }
        std::memcpy(&out, payload.data.bytes.data(), sizeof(T));
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

    PrintVideoRecorder print_video_;
    PrintH264Recorder print_h264_;
    CleanVideoRecorder clean_video_;
};

} // namespace upc::media

#endif
