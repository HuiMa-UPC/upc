#ifndef UPC_VISUAL_SERVICE_HPP
#define UPC_VISUAL_SERVICE_HPP

#include "first_layer_detector.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "msg_port.hpp"

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace upc::visual {

/**
 * @brief 视觉服务：并行订阅相机预览流并执行首层就绪检测。
 *
 * 数据链路：
 * 1) 通过 IPC 请求 Camera 建立 visual 专用流会话
 * 2) 从 SHM 元数据环获取最新帧索引（buffer_idx）
 * 3) 后台线程消费并释放帧槽，并缓存最新紧凑 NV12 原图
 * 4) 收到 Glavo 首层检测请求时，使用 Y 平面运行模型并回传 0/1 结果
 */
class VisualService {
public:
    VisualService() = default;
    ~VisualService();

    VisualService(const VisualService&) = delete;
    VisualService& operator=(const VisualService&) = delete;

    bool init();
    void shutdown();
    /** 周期驱动：仅在首层打印会话激活且流未就绪时重发 start 请求。 */
    void tick();

private:
    struct FrameSnapshot {
        std::vector<uint8_t> nv12;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frame_seq = 0;
    };

    /** 注册视觉流起流响应回调。 */
    void subscribe_messages();
    /** 请求 Camera 开启 visual 并行流。 */
    void send_stream_start_req();
    /** 请求 Camera 关闭 visual 并行流。 */
    void send_stream_stop_req();
    /** 处理 APP_Visual_Camera_Stream_Start_Rsp，并拉起 worker。 */
    void on_stream_start_rsp(const middleware::Message& msg);
    /** 打印开始：起流并预热定位缓存；重复通知保持幂等。 */
    void on_first_layer_session_start(const middleware::Message& msg);
    /** 首层通过/打印结束：停流并释放 Camera 资源，保留 RKNN 上下文。 */
    void on_first_layer_session_stop(const middleware::Message& msg);
    void start_first_layer_session(const char* reason);
    void stop_first_layer_session(const char* reason);
    void send_reference_ready_rsp(bool ready, uint32_t frame_seq,
                                  const std::string& reason);
    void try_capture_bare_reference(const FrameSnapshot& frame);
    void clear_latest_snapshot();
    /** 处理 GLAVO_Visual_FirstLayer_Check_Req，回传检测结果。 */
    void on_first_layer_check_req(const middleware::Message& msg);
    /** 停止后台线程并重置流状态。 */
    void stop_worker();
    /** 后台线程主循环：取最新帧、做 NV12 拷贝并释放槽位。 */
    void worker_thread_fn();
    bool map_dma_views_if_needed();
    void unmap_dma_views();
    void update_latest_snapshot(const upc_ipc::IpcDmaFrameView& frame);
    bool copy_latest_snapshot(std::vector<uint8_t>& out, uint32_t& width,
                              uint32_t& height, uint32_t& frame_seq);
    bool copy_recent_snapshots(std::vector<FrameSnapshot>& out);
    bool process_first_layer_frames(const std::vector<FrameSnapshot>& frames);
    /** Require consecutive accepted checks; any rejected check resets the count. */
    uint32_t update_consecutive_passes(bool candidate_pass, uint32_t frame_seq);
    void cache_final_rejected_diagnostic(const std::vector<uint8_t>& nv12,
                                         uint32_t width, uint32_t height,
                                         uint32_t frame_seq,
                                         const FirstLayerDetection& detection);
    static void copy_nv12_tight(uint8_t* dst, const uint8_t* src_y,
                                const uint8_t* src_uv, uint32_t src_stride_y,
                                uint32_t width, uint32_t height);

    static bool read_inline_start_rsp(const middleware::Message& msg,
                                      middleware::CameraStreamInfoPayload& out);
    static uint64_t monotonic_ms();

    std::vector<middleware::Subscription> subs_;
    upc_ipc::IpcDmaFrameRingConsumer consumer_;
    std::vector<int> dma_fds_;
    std::vector<uint8_t*> dma_maps_;
    size_t dma_map_size_ = 0;

    /** 保护 start/stream 状态，避免 tick 与消息回调竞争。 */
    std::mutex state_mtx_;
    bool first_layer_session_active_ = false;
    uint64_t first_layer_session_id_ = 0;
    bool first_layer_session_passed_ = false;
    uint32_t first_layer_consecutive_passes_ = 0;
    uint32_t first_layer_last_pass_frame_seq_ = 0;
    bool reference_capture_pending_ = false;
    uint64_t last_reference_capture_attempt_ms_ = 0;
    uint64_t last_reference_failure_report_ms_ = 0;
    std::string last_reference_failure_reason_;
    bool start_req_sent_ = false;
    bool stream_up_ = false;
    bool pending_start_rsp_valid_ = false;
    middleware::CameraStreamInfoPayload pending_start_rsp_{};
    std::mutex frame_mtx_;
    std::vector<uint8_t> latest_nv12_snapshot_;
    uint32_t latest_width_ = 0;
    uint32_t latest_height_ = 0;
    uint32_t latest_frame_seq_ = 0;
    bool latest_nv12_valid_ = false;
    std::deque<FrameSnapshot> recent_snapshots_;
    uint32_t last_temporal_frame_seq_ = 0;
    /** 串行化后台定位审计与检测请求，RKNN/定位缓存不是并发接口。 */
    std::mutex detector_mtx_;
    FirstLayerDetector first_layer_detector_;
    uint32_t last_geometry_log_frame_ = 0;
    std::string last_geometry_log_event_;
    std::string last_geometry_log_reason_;

    /** 本会话最近一次有效 result=0；会话结束仍未通过时只保存这一张诊断图。 */
    struct RejectedDiagnosticCandidate {
        bool valid = false;
        std::vector<uint8_t> nv12;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t frame_seq = 0;
        FirstLayerDetection detection;
    };
    std::mutex rejected_diagnostic_mtx_;
    RejectedDiagnosticCandidate latest_rejected_diagnostic_;

    /** 后台取帧线程。 */
    std::thread worker_;
    std::atomic<bool> stop_worker_{false};

    /** 起流重试时间戳。 */
    uint64_t last_start_req_ms_ = 0;
};

} // namespace upc::visual

#endif
