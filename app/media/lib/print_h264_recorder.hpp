#ifndef PRINT_H264_RECORDER_HPP
#define PRINT_H264_RECORDER_HPP

#include "device_config.hpp"
#include "msg_runtime.hpp"

#include <cstdint>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace upc::media {

/**
 * 打印期连续录像（H.264 裸流）记录器。
 *
 * 职责：
 * - 媒体消费线程只负责把 NV12 帧拷贝后入队，不做重编码等耗时操作，避免阻塞上游。
 * - 后台 worker 线程异步将 NV12 帧喂给 RKMPP 编码器，输出 H.264 裸流。
 * - 直接输出到 /home/media/video/tmp/print.h264（H.264 裸流，无封装）。
 * - 当写盘队列满时主动丢帧，保预览实时性，不阻塞媒体流消费。
 *
 * 生命周期：
 * - on_session_start：开始一次新的打印期录像，清空上一次状态，启动 worker。
 * - on_frame_nv12：每帧 NV12 回调，快速拷贝并入队。
 * - on_session_finish / on_session_abort：结束或中止本次打印期录像，由 worker 关闭编码器、写文件尾。
 */
class PrintH264Recorder {
public:
    PrintH264Recorder() = default;
    ~PrintH264Recorder();

    PrintH264Recorder(const PrintH264Recorder&) = delete;
    PrintH264Recorder& operator=(const PrintH264Recorder&) = delete;

    /** 设置编码流的分辨率和帧率，任何一项为 0 时保持当前值。 */
    void set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps);
    /** 收到新的打印期会话开始事件，初始化会话路径并启动编码流程。 */
    void on_session_start(const middleware::PrintVideoSessionPayload& info);
    /** 正常结束打印期会话，由后台 worker 完成编码器收尾和文件落盘。 */
    void on_session_finish(const middleware::PrintVideoSessionPayload& info);
    /** 异常中止打印期会话，仍保留已编码数据，除非没有任何帧才清理。 */
    void on_session_abort(const middleware::PrintVideoSessionPayload& info);
    /** 设置编码器已排空并关闭输出文件后的回调；回调在 worker 线程执行。 */
    void set_finalize_done_callback(std::function<void()> cb);
    /** 基于最后一个成功编码的帧生成一张缩略图。 */
    bool create_final_thumb(const char* out_path) const;
    /** 暂停或恢复录像；暂停期间到达的帧直接丢弃。 */
    void set_paused(bool paused);
    /** 每帧 NV12 回调入口；会拷贝数据并入队到 worker 线程做编码。 */
    void on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y);
    /** 清理固定临时目录内残留的 print.h264 / print_h264.mp4。 */
    void enqueue_cleanup_tmp();
    /** 设置写盘前的存储空间预算检查函数；返回 false 时跳过本帧写入。 */
    void set_storage_budget_guard(std::function<bool()> cb);
    /** 返回当前活跃会话的临时目录；无活跃会话返回空字符串。 */
    std::string current_session_dir() const;
    /** 是否正处于打印期录像中。 */
    bool session_active() const;

private:
    /** 后台工作队列中任务类型。 */
    enum class JobKind : uint8_t {
        WriteFrame = 0, /**< 写入一帧 NV12 并编码。 */
        Finalize   = 1, /**< 结束会话，关闭编码器和文件。 */
        Cleanup    = 2, /**< 清理临时目录。 */
    };

    /** 工作队列任务，根据 kind 解释不同字段含义。 */
    struct Job {
        JobKind kind = JobKind::WriteFrame; /**< 任务类型。 */
        uint64_t session_seq = 0;           /**< 所属会话序号，用于过滤旧会话任务。 */
        /* WriteFrame 专用 */
        uint32_t width = 0;                 /**< 帧宽度。 */
        uint32_t height = 0;                /**< 帧高度。 */
        std::vector<uint8_t> nv12;          /**< 紧凑 NV12 数据（Y 接 UV）。 */
        /* Finalize 专用 */
        bool aborted = false;               /**< 是否异常中止。 */
        std::string session_id;             /**< 会话 ID，仅用于日志。 */
        /* Cleanup 专用 */
        std::string cleanup_dir;            /**< 需要清理的目录。 */
    };

    /** 入队一个 Finalize 任务；调用前必须持有 mtx_。 */
    void enqueue_finalize_locked(bool aborted);
    /** 清空会话相关路径字符串；调用前必须持有 mtx_。 */
    void clear_session_paths_locked();
    /** 重置一次会话的运行时状态；调用前必须持有 mtx_。 */
    void reset_session_state_locked();
    /** 若 worker 线程未启动则启动；调用前必须持有 mtx_。 */
    void start_worker_if_needed_locked();
    /** 通知 worker 停止并等待其结束。 */
    void stop_worker();
    /** 后台 worker 主循环，从队列中取出任务并分发给处理器。 */
    void worker_fn();
    /** 打开 RKMPP 编码器并配置参数；调用前必须持有 mtx_。 */
    bool open_encoder_locked();
    /** 关闭 RKMPP 编码器并冲刷残码流到文件；调用前必须持有 mtx_。 */
    int close_encoder_locked();
    /** 处理 WriteFrame 任务：编码一帧并写入 H.264 文件。 */
    void run_write_frame_job(const Job& job);
    /** 处理 Finalize 任务：关闭编码器、落盘、日志并触发回调。 */
    void run_finalize_job(Job job);
    /** 处理 Cleanup 任务：删除临时目录下指定文件。 */
    void run_cleanup_job(const Job& job);
    /** 将非安全字符替换为下划线，生成可用作路径名的字符串。 */
    static std::string sanitize_filename(const char* name);
    /** 统计队列中待处理的 WriteFrame 任务数量。 */
    static size_t count_pending_write_frames(const std::deque<Job>& jobs);

    mutable std::mutex mtx_;              /**< 保护本类所有可变成员。 */
    std::condition_variable cv_;          /**< 通知 worker 有新的任务。 */
    std::condition_variable finalize_cv_; /**< 通知 finalize 完成（当前未单独使用）。 */
    std::condition_variable idle_cv_;     /**< 通知会话启动时 worker 已空闲。 */
    std::deque<Job> jobs_;                /**< 待处理任务队列。 */
    std::thread worker_;                /**< 后台工作线程。 */
    bool worker_stop_ = false;          /**< worker 停止标志。 */
    bool worker_busy_ = false;          /**< worker 是否正在处理任务。 */
    uint64_t worker_busy_session_ = 0;  /**< worker 当前处理任务所属会话序号。 */

    bool active_ = false;               /**< 当前是否有活跃打印期会话。 */
    bool paused_ = false;               /**< 是否暂停录像。 */
    bool had_write_error_ = false;      /**< 本次会话是否发生过写盘或编码错误。 */
    bool finalize_in_progress_ = false; /**< 是否有 Finalize 任务正在执行。 */
    uint64_t session_seq_ = 0;          /**< 会话序号，每次 start 自增。 */
    uint32_t width_ = CAMERA_WIDTH_DEFAULT;   /**< 编码宽度。 */
    uint32_t height_ = CAMERA_HEIGHT_DEFAULT; /**< 编码高度。 */
    uint32_t fps_ = CAMERA_FPS_DEFAULT;      /**< 编码帧率。 */
    uint32_t bitrate_bps_ = CAMERA_PRINT_H264_BITRATE; /**< 目标码率。 */
    uint32_t frame_count_ = 0;          /**< 本次会话成功编码的帧数。 */
    uint32_t dropped_frames_ = 0;       /**< 本次会话因队列满丢弃的帧数。 */
    std::vector<uint8_t> last_nv12_;    /**< 最后一帧成功编码的 NV12 数据，用于缩略图。 */

    std::string session_id_;            /**< 当前会话 ID。 */
    std::string session_name_;          /**< 当前会话名称（已安全化）。 */
    std::string session_dir_;           /**< 当前会话输出目录。 */
    std::string h264_path_;             /**< 当前 H.264 输出文件完整路径。 */
    FILE* h264_fp_ = nullptr;           /**< 当前 H.264 输出文件句柄。 */

    /* RKMPP 对象（避免在头文件引入 RK 头文件，使用 void* 封装）。 */
    void* mpp_ctx_ = nullptr;           /**< RKMPP 编码器上下文。 */
    void* mpp_mpi_ = nullptr;           /**< RKMPP API 接口指针。 */
    void* mpp_cfg_ = nullptr;           /**< RKMPP 编码配置对象。 */
    bool mpp_ready_ = false;            /**< 编码器是否已成功打开。 */
    std::function<void()> finalize_done_cb_;     /**< 完成回调。 */
    std::function<bool()> storage_budget_guard_; /**< 存储预算检查回调。 */
};

} // namespace upc::media

#endif /* PRINT_H264_RECORDER_HPP */
