#ifndef H264_STREAM_ENCODER_HPP
#define H264_STREAM_ENCODER_HPP

#include "device_config.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace upc::media {

/**
 * 直播路径共用的 RKMPP H.264 编码器（单实例，多 sink 分发）。
 *
 * 数据路径：
 *   Media DMA 消费线程 on_frame_nv12()
 *     → 单槽 pending_nv12_（depth=1，latest-wins；积压则丢旧帧）
 *     → worker：RKMPP 编 Annex-B H.264
 *     → 依次交给每个已注册 sink（云端推流 / 本地 RTSP Server）
 *
 * 设计约束：
 * - 云端推流与本地 RTSP Server 共用同一路码流，只编一次；
 *   编码参数（分辨率/帧率/码率）由**首个** sink 决定，后续 sink 沿用。
 * - sink 之间互不连坐：某一路投递失败只摘该路，其余继续；
 *   只有 MPP 自身失败才停整个编码会话。
 * - 采帧路径只做紧凑 NV12 拷贝，编码与投递都在 worker，避免拖慢 DMA 环。
 * - 落盘用的 PrintH264Recorder 不走本类（码率/GOP 诉求不同，且有暂停与存储水位逻辑）。
 */
class H264StreamEncoder {
public:
    /**
     * 每个编码包（Annex-B，可能含多个 NAL）的投递回调。
     * 返回 false 表示该路已失效（如管道断开），Encoder 会摘掉它并回调 ErrorCb。
     * 禁止在本回调内调用 remove_sink()（会自等死锁）。
     *
     * pts_us 是**采帧时刻**的墙钟微秒，同一输入帧产生的所有包共用同一值：
     * MPP 可能把一帧拆成多个包（EACH_IDR 下参数集常自成一包），若各包各自
     * 取时刻，live555 会因 presentation time 不同而误判 AU 边界、marker bit
     * 置错，客户端表现为 "missing picture in access unit"。
     */
    using PacketCb = std::function<bool(const uint8_t* annexb, size_t len,
                                        bool keyframe, int64_t pts_us)>;
    /** sink 被 Encoder 摘除后的通知（worker 线程，未持锁）。 */
    using ErrorCb = std::function<void()>;

    struct SinkOptions {
        bool request_idr_on_join = true; /**< 接入时请求立即 IDR，免等一个 GOP */
    };

    H264StreamEncoder() = default;
    ~H264StreamEncoder();

    H264StreamEncoder(const H264StreamEncoder&) = delete;
    H264StreamEncoder& operator=(const H264StreamEncoder&) = delete;

    /** 设置编码几何；任一项为 0 时保持当前值。开流后由 MediaService 同步。 */
    void set_stream_geometry(uint32_t width, uint32_t height, uint32_t fps);

    /**
     * 注册一路 sink。sink 数 0→1 时打开 MPP 并启动 worker。
     * 码率固定为 CAMERA_PRINT_H264_BITRATE，不随 sink 变化。
     * @return sink 句柄；几何未知或 MPP 打不开返回 -1。
     */
    int add_sink(PacketCb on_packet, ErrorCb on_error, const SinkOptions& opt);

    /**
     * 摘除一路 sink（句柄无效时静默返回，可幂等调用）。
     * 返回后保证该 sink 的 PacketCb 不会再被调用；sink 数 1→0 时关闭 MPP。
     */
    void remove_sink(int handle);

    /**
     * 每帧 NV12 入口（DMA 消费线程调用）。
     * 有 sink 时拷贝入单槽 pending；若上一帧尚未被 worker 取走则覆盖并 ++dropped_。
     */
    void on_frame_nv12(const uint8_t* src_y, const uint8_t* src_uv, uint32_t stride_y);

    /**
     * 取最近一次 IDR 携带的参数集（不含起始码），供 live555 填 SDP
     * 的 sprop-parameter-sets。首个 IDR 之前返回 false。
     */
    bool copy_sps_pps(std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) const;

    /** 请求下一帧编为 IDR（新 sink 接入时用，免等一个 GOP）。 */
    void request_idr();

    /** 当前是否已有 sink（即编码会话是否活跃）。 */
    [[nodiscard]] bool active() const;

    /**
     * 遍历 Annex-B 码流中的 NAL，回调收到的是**不含起始码**的裸 NAL。
     * 供本类缓存参数集，也供 LocalRtspServer 拆包喂 live555。
     */
    static void for_each_nal(const uint8_t* annexb, size_t len,
                             const std::function<void(const uint8_t*, size_t)>& fn);

private:
    /** 一路投递目标；用 shared_ptr 持有，保证 worker 分发期间不被析构。 */
    struct SinkEntry {
        int               id = 0;
        PacketCb          on_packet;
        ErrorCb           on_error;
        std::atomic<bool> alive{true};
    };

    /** join worker 并清 worker_started_；调用前须已置 worker_stop_ 并 notify。 */
    void stop_worker();
    /** 后台主循环：取 pending → 编码 → 分发；摘除失败 sink。 */
    void worker_fn();
    /** 打开/复用 RKMPP H.264 编码器；调用前必须持有 mtx_。 */
    bool open_encoder_locked();
    /** 冲刷 EOS 并销毁 RKMPP；调用前必须持有 mtx_。 */
    void close_encoder_locked();
    /** 从 sinks_ 摘除并置 alive=false；调用前必须持有 mtx_。 */
    std::shared_ptr<SinkEntry> detach_sink_locked(int handle);
    /** sink 清空后的收尾：关编码器、丢弃 pending；调用前必须持有 mtx_。 */
    void shutdown_if_no_sink_locked();
    /**
     * 编码一帧并分发给 snapshot 出来的 sink 列表。
     * 锁外执行：MPP put/get 与各 sink 回调都可能阻塞。
     * @param pts_us 本帧采帧时刻，透传给所有 sink。
     * @param failed 出参，收集投递失败的 sink。
     * @param out_bytes 出参，本帧编码输出的总字节数（心跳统计用）。
     * @return false 表示 MPP 自身失败（编码器级故障，应停整个会话）。
     */
    bool encode_and_dispatch(void* ctx, void* mpi,
                             const std::vector<uint8_t>& nv12,
                             uint32_t w, uint32_t h, int64_t pts_us,
                             const std::vector<std::shared_ptr<SinkEntry>>& sinks,
                             std::vector<std::shared_ptr<SinkEntry>>& failed,
                             size_t& out_bytes);
    /** 距上次心跳满 kStatsPeriodUs 则打一行统计；调用前必须持有 mtx_。 */
    void maybe_log_stats_locked();
    /** 扫描包内 NAL：缓存 SPS/PPS，并判定是否含 IDR。 */
    bool scan_packet(const uint8_t* annexb, size_t len);

    mutable std::mutex mtx_;              /**< 保护 sink 表、pending、MPP 句柄、参数集。 */
    std::condition_variable cv_;          /**< 通知 worker：有新帧或应停止。 */
    std::condition_variable dispatch_cv_; /**< 通知 remove_sink：本轮分发已结束。 */
    std::thread worker_;                  /**< 编码 + 分发后台线程。 */
    bool worker_stop_ = false;            /**< 析构时置位，worker 退出循环。 */
    bool worker_started_ = false;         /**< worker 是否已启动（进程内复用同一线程）。 */
    bool dispatching_ = false;            /**< worker 是否正在锁外回调 sink。 */

    std::vector<std::shared_ptr<SinkEntry>> sinks_;
    int next_sink_id_ = 1;

    uint32_t width_ = CAMERA_WIDTH_DEFAULT;
    uint32_t height_ = CAMERA_HEIGHT_DEFAULT;
    uint32_t fps_ = CAMERA_FPS_DEFAULT;

    bool has_pending_ = false;            /**< pending_nv12_ 是否有待编码帧。 */
    std::vector<uint8_t> pending_nv12_;   /**< 单槽最新帧（紧凑 NV12）。 */
    uint32_t pending_w_ = 0;
    uint32_t pending_h_ = 0;
    int64_t  pending_pts_us_ = 0;         /**< 单槽帧的采帧时刻（墙钟微秒）。 */
    uint32_t dropped_ = 0;                /**< 因单槽覆盖而丢弃的旧帧计数。 */

    /* 运行期心跳统计：故障时靠这几个计数是否还在涨定位断点。 */
    static constexpr int64_t kStatsPeriodUs = 5000000; /**< 心跳周期 5s。 */
    uint64_t stat_in_frames_ = 0;         /**< 累计进入 on_frame_nv12 的帧数。 */
    uint64_t stat_enc_frames_ = 0;        /**< 累计成功编码的帧数。 */
    uint64_t stat_enc_bytes_ = 0;         /**< 累计编码输出字节数。 */
    int64_t  stat_last_log_us_ = 0;       /**< 上次心跳打印时刻；0=尚未打过。 */
    uint64_t stat_last_in_ = 0;           /**< 上次心跳时的 stat_in_frames_。 */
    uint64_t stat_last_enc_ = 0;          /**< 上次心跳时的 stat_enc_frames_。 */
    uint64_t stat_last_bytes_ = 0;        /**< 上次心跳时的 stat_enc_bytes_。 */

    std::vector<uint8_t> sps_;            /**< 最近一次 IDR 的 SPS（不含起始码）。 */
    std::vector<uint8_t> pps_;            /**< 最近一次 IDR 的 PPS（不含起始码）。 */

    /* RKMPP 用 void* 封装，避免头文件依赖 rk_mpi.h。 */
    void* mpp_ctx_ = nullptr;
    void* mpp_mpi_ = nullptr;
    void* mpp_cfg_ = nullptr;
    bool mpp_ready_ = false;
};

} // namespace upc::media

#endif /* H264_STREAM_ENCODER_HPP */
