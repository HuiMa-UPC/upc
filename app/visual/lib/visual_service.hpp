#ifndef UPC_VISUAL_SERVICE_HPP
#define UPC_VISUAL_SERVICE_HPP

#include "first_layer_detector.hpp"
#include "ipc_dma_frame_ring.hpp"
#include "msg_port.hpp"
#include "red_dot_detector.hpp"
#include "plate_height_pipeline.hpp"
#include "cabin_detector.hpp"

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
 *
 * 四类会话共用同一路取流（首层 / 光路 / 测高 / 舱内检测）。取流的起停按
 * 「任一会话激活即保持」处理，任一会话单独结束不得把流拆掉，
 * 见 any_session_active_locked() 与 release_stream_if_idle()。
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
    /**
     * 起流回执没接上。Camera 侧 peer 可能已经在提交帧，这里只清本地标志让
     * tick 重试，peer 留到会话结束由 release_stream_if_idle 发 Stop。
     */
    void note_stream_attach_failed(const char* why);
    /** 取帧线程已在跑且流标记为就绪。调用方不得持有 state_mtx_。 */
    bool stream_is_serving();
    /** 打印开始：起流并预热定位缓存；重复通知保持幂等。 */
    void on_first_layer_session_start(const middleware::Message& msg);
    /** 首层通过/打印结束：停流并释放 Camera 资源，保留 RKNN 上下文。 */
    void on_first_layer_session_stop(const middleware::Message& msg);
    void start_first_layer_session(const char* reason);
    void stop_first_layer_session(const char* reason);
    /** 是否有会话需要取流；调用前须持有 state_mtx_。 */
    [[nodiscard]] bool any_session_active_locked() const;
    void on_plate_height_session_start(const middleware::Message& msg);
    void on_plate_height_session_stop(const middleware::Message& msg);
    void start_plate_height_session(const char* reason);
    void stop_plate_height_session(const char* reason);
    void on_laser_spot_measure_req(const middleware::Message& msg);
    /**
     * 光斑取样窗口（跑在 laser_spot_thread_ 上）。
     * 先等本点之后的新快照（最多约 8s，旧帧和空快照不算没光斑），
     * 再在约 1.5s 内认质心。不能在消息回调里空等，
     * 否则起流回执进不来，快照会一直是空的。
     */
    void run_laser_spot_window();
    void stop_laser_spot_thread();
    void send_laser_spot_measure_rsp(bool ok, float x, float y);
    /** 所有会话都不再需要流时才拆流。 */
    void release_stream_if_idle(const char* reason);
    void send_reference_ready_rsp(bool ready, uint32_t frame_seq,
                                  const std::string& reason);
    void try_capture_bare_reference(const FrameSnapshot& frame);
    void clear_latest_snapshot();
    /** 处理 GLAVO_Visual_FirstLayer_Check_Req，回传检测结果。 */
    void on_first_layer_check_req(const middleware::Message& msg);

    /* ---- 打印前光路检测 ---- */
    void on_optical_path_session_start(const middleware::Message& msg);
    void on_optical_path_session_stop(const middleware::Message& msg);
    void start_optical_path_session(const char* reason);
    void stop_optical_path_session(const char* reason);
    /** 处理 GLAVO_Visual_OpticalPath_Point_Check_Req：起一条取点线程做窗口轮询。 */
    void on_optical_path_point_check_req(const middleware::Message& msg);
    /**
     * 单点判定窗口轮询体（跑在 optical_point_thread_ 上）。
     * 先等快照就绪（最多约 8s），再在 timeout_ms 内逐帧调 RedDotDetector。
     * 空快照不计入「没找到红点」。命中即回包；无帧回 fail_code=1，有帧未找到回 0。
     * 必须另起线程：消息分发跑在 main.cpp 的主循环里（port.process + tick），
     * 在回调里阻塞会连 tick 与起流应答一起卡住。
     */
    void run_optical_point_window(
        const middleware::VisualOpticalPathCheckReqPayload& req);
    /**
     * @param fail_code result=0 时写入 reserved[0]：1=整段无快照，0=检测器已跑仍未找到。
     */
    void send_optical_path_point_rsp(
        const middleware::VisualOpticalPathCheckReqPayload& req,
        const RedDotDetection& detection,
        uint8_t fail_code);
    /** 回收取点线程。 */
    void stop_optical_point_thread();

    void on_cabin_session_start(const middleware::Message& msg);
    void on_cabin_session_stop(const middleware::Message& msg);
    void on_cabin_check_req(const middleware::Message& msg, bool door);
    void run_cabin_window(middleware::VisualCabinCheckReqPayload req, bool door,
                          uint32_t baseline_seq);
    void stop_cabin_session(const char* reason);
    void interrupt_cabin_for_legacy();
    void drain_cabin_result();
    void send_cabin_rsp(bool door, const middleware::VisualCabinCheckRspPayload& rsp);

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
    void cache_final_rejected_diagnostic(const std::vector<uint8_t>& nv12,
                                         uint32_t width, uint32_t height,
                                         uint32_t frame_seq,
                                         const FirstLayerDetection& detection);
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
    bool reference_capture_pending_ = false;
    uint64_t last_reference_capture_attempt_ms_ = 0;
    uint64_t last_reference_failure_report_ms_ = 0;
    std::string last_reference_failure_reason_;
    /** 光路检测会话：与首层会话独立开关，共用同一路取流。 */
    bool optical_path_session_active_ = false;
    uint64_t optical_path_session_id_ = 0;
    bool plate_height_session_active_ = false;
    uint64_t plate_height_session_id_ = 0;
    bool cabin_session_active_ = false;
    bool cabin_stream_needed_ = false;
    uint32_t cabin_session_id_ = 0;
    uint64_t cabin_idle_deadline_ms_ = 0;
    uint64_t cabin_stopped_ms_ = 0;
    bool start_req_sent_ = false;
    /**
     * 已经向 Camera 要过 visual peer。起流回执失败时 peer 仍在提交帧，
     * 只清 stream_up_ / start_req_sent_ 的话，会话结束不会发 Stop，
     * 没人 release 的槽会把 UI 预览一起卡住。
     */
    bool camera_peer_requested_ = false;
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

    /** 红点识别器（算法桩，见 red_dot_detector.hpp）。 */
    RedDotDetector red_dot_detector_;
    /** 测高/校准光斑取样线程。Glavo 逐点串行，同一时刻只有一条。 */
    std::thread laser_spot_thread_;
    std::atomic<bool> laser_spot_abort_{false};
    /** 单点判定窗口轮询线程；Glavo 逐点串行，同一时刻只有一条。 */
    std::thread optical_point_thread_;
    std::atomic<bool> optical_point_abort_{false};

    CabinDetector cabin_detector_;
    std::thread cabin_thread_;
    std::atomic<bool> cabin_abort_{false};
    // Request bookkeeping lives on the IPC thread. Only the pending result is shared.
    bool cabin_check_active_ = false;
    bool cabin_check_door_ = false;
    middleware::VisualCabinCheckReqPayload cabin_request_{};
    bool cabin_cached_valid_ = false;
    bool cabin_cached_door_ = false;
    middleware::VisualCabinCheckRspPayload cabin_cached_rsp_{};
    std::mutex cabin_result_mtx_;
    bool cabin_result_pending_ = false;
    middleware::VisualCabinCheckRspPayload cabin_pending_rsp_{};

    /** 后台取帧线程。 */
    std::thread worker_;
    std::atomic<bool> stop_worker_{false};

    /** 起流重试时间戳。 */
    uint64_t last_start_req_ms_ = 0;
};

} // namespace upc::visual

#endif
