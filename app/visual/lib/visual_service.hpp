#ifndef UPC_VISUAL_SERVICE_HPP
#define UPC_VISUAL_SERVICE_HPP

#include "ipc_dma_frame_ring.hpp"
#include "msg_port.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace upc::visual {

/**
 * @brief 视觉占位服务：并行订阅相机预览流并持续消费最新帧。
 *
 * 数据链路：
 * 1) 通过 IPC 请求 Camera 建立 visual 专用流会话
 * 2) 从 SHM 元数据环获取最新帧索引（buffer_idx）
 * 3) 占位线程消费并释放帧槽（不做识别运算、不向 UI 上报）
 */
class VisualService {
public:
    VisualService() = default;
    ~VisualService();

    VisualService(const VisualService&) = delete;
    VisualService& operator=(const VisualService&) = delete;

    bool init();
    void shutdown();
    /** 周期驱动：流未就绪时按节流策略重发 start 请求。 */
    void tick();

private:
    /** 注册视觉流起流响应回调。 */
    void subscribe_messages();
    /** 请求 Camera 开启 visual 并行流。 */
    void send_stream_start_req();
    /** 请求 Camera 关闭 visual 并行流。 */
    void send_stream_stop_req();
    /** 处理 APP_Visual_Camera_Stream_Start_Rsp，并拉起 worker。 */
    void on_stream_start_rsp(const middleware::Message& msg);
    /** 停止后台线程并重置流状态。 */
    void stop_worker();
    /** 占位线程主循环：取最新帧并释放槽位。 */
    void worker_thread_fn();

    static bool read_inline_start_rsp(const middleware::Message& msg,
                                      middleware::CameraStreamInfoPayload& out);
    static uint64_t monotonic_ms();

    std::vector<middleware::Subscription> subs_;
    upc_ipc::IpcDmaFrameRingConsumer consumer_;
    std::vector<int> dma_fds_;

    /** 保护 start/stream 状态，避免 tick 与消息回调竞争。 */
    std::mutex state_mtx_;
    bool start_req_sent_ = false;
    bool stream_up_ = false;
    bool pending_start_rsp_valid_ = false;
    middleware::CameraStreamInfoPayload pending_start_rsp_{};

    /** 后台占位线程。 */
    std::thread worker_;
    std::atomic<bool> stop_worker_{false};

    /** 起流重试时间戳。 */
    uint64_t last_start_req_ms_ = 0;
};

} // namespace upc::visual

#endif
