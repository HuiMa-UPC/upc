#ifndef LOCAL_RTSP_SERVER_HPP
#define LOCAL_RTSP_SERVER_HPP

#include "h264_stream_encoder.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/** 本地 RTSP 服务端口（局域网拉流；与 xfer 的 8080 无冲突）。 */
#ifndef UPC_LOCAL_RTSP_PORT_DEFAULT
#define UPC_LOCAL_RTSP_PORT_DEFAULT 8554
#endif

/** 本地 RTSP 流名：rtsp://<ip>:<port>/live */
#ifndef UPC_LOCAL_RTSP_STREAM_NAME
#define UPC_LOCAL_RTSP_STREAM_NAME "live"
#endif

/**
 * 上电是否默认启用本地 RTSP（调试页 Other → Local RTSP 开关的初值）。
 * 实际拉起时机是相机流就绪之后——开流前编码几何未知，start() 必失败。
 * 置 0 则恢复"上电关闭、需手动打开"。
 */
#ifndef UPC_LOCAL_RTSP_AUTO_START
#define UPC_LOCAL_RTSP_AUTO_START 1
#endif

/**
 * NAL 队列上限（个）。超出时整段丢到下一个 IDR，不丢单个 NAL：
 * 半个 AU / 半个 GOP 会让解码器灰屏，误差沿 P 帧传到下一个关键帧。
 * 约 1s（30fps、每帧 1～2 个 NAL）；再长只增加端到端延迟。
 */
#ifndef UPC_LOCAL_RTSP_QUEUE_MAX
#define UPC_LOCAL_RTSP_QUEUE_MAX 48u
#endif

/**
 * live555 单帧输出缓冲上限（字节，`OutPacketBuffer::maxSize`）。
 *
 * 库默认值只有 60000，而 1080×1280 的 I 帧在 2.5Mbps CBR 下可达 150～200KB，
 * 超出部分会被 RTPSink 静默丢弃，客户端每个 I 帧都花屏且误差沿 P 帧传播到下一个 GOP。
 */
#ifndef UPC_LOCAL_RTSP_MAX_FRAME_BYTES
#define UPC_LOCAL_RTSP_MAX_FRAME_BYTES 1000000u
#endif

namespace upc::media {

/**
 * 基于 live555 的本地局域网 RTSP Server（桌面端/安卓端同网拉流）。
 *
 * 数据路径：
 *   H264StreamEncoder（与云端推流共用同一路码流）
 *     → 本类注册的 sink 回调：拆 Annex-B 为裸 NAL 入队 + triggerEvent
 *     → live555 事件循环线程：H264FramedSource → H264VideoStreamDiscreteFramer
 *       → H264VideoRTPSink → 客户端
 *
 * 线程模型（务必遵守）：
 * - live555 事件循环随 Media 进程常驻，不随开关销毁（反复销毁 env/scheduler 易踩回收陷阱）。
 * - 除 triggerEvent() 外，所有 live555 API 只能在事件循环线程内调用；
 *   开关的挂/摘 session 通过命令队列 + trigger 投递到该线程执行。
 * - 开关只控制 ServerMediaSession 的挂载与编码器 sink 的增删。
 *
 * 一期不做鉴权（局域网内可直接拉流），地址仅打日志、不在 UI 展示。
 */
class LocalRtspServer {
public:
    /** 入队的一个裸 NAL（不含起始码）。 */
    struct NalUnit {
        std::vector<uint8_t> data;
        int64_t pts_us = 0;   /**< 墙钟微秒；同一编码包内的 NAL 共用同一时间戳。 */
    };

    /** 一个可用于局域网拉流的地址。 */
    struct LanAddr {
        std::string iface;    /**< 接口名，如 wlan0 / eth0 */
        std::string ip;       /**< 点分十进制 IPv4 */
    };

    LocalRtspServer() = default;
    ~LocalRtspServer();

    LocalRtspServer(const LocalRtspServer&) = delete;
    LocalRtspServer& operator=(const LocalRtspServer&) = delete;

    /** 绑定共用编码器；必须在 start() 之前由 MediaService 注入。 */
    void set_encoder(H264StreamEncoder* encoder);

    /**
     * 建立 RTSPServer 并启动常驻事件循环线程（进程内只需一次）。
     * 必须成功绑定 IPv4；仅 IPv6 不算起来（Windows/Snowpath 走 IPv4）。
     * 未编译 live555 支持或 IPv4 端口占用时返回 false，调用方可稍后重试。
     */
    bool init(uint16_t port = UPC_LOCAL_RTSP_PORT_DEFAULT);

    /** 停事件循环并释放 live555 资源（进程退出时调用）。 */
    void shutdown();

    /**
     * 打开本地直播：注册编码器 sink 并挂载 ServerMediaSession。
     * 相机流未就绪（几何未知）或 RKMPP 打不开时返回 false，调用方据此把开关弹回。
     */
    bool start();

    /** 关闭本地直播：摘 sink、断开在连客户端并撤下 session。幂等。 */
    void stop();

    /** 当前是否处于开启状态。 */
    [[nodiscard]] bool active() const;

    /** rtsp://<首选IP>:<port>/live；无可用地址时回落到 0.0.0.0。 */
    [[nodiscard]] std::string url() const;

    /**
     * 枚举可用于局域网拉流的 IPv4，按 WiFi → 有线 → 其它 排序，首项即 url() 采用的地址。
     *
     * 跳过 loopback、DOWN、虚拟网卡（dummy/docker/veth/…）与 169.254.0.0/16
     * 链路本地地址——后两类在多网口设备上会排在真实网卡前面，直接取"第一个非
     * loopback 接口"会得到客户端根本连不上的地址。
     */
    [[nodiscard]] static std::vector<LanAddr> lan_ipv4_list();

    /* ---- 以下仅供 cpp 内的 live555 source 使用（事件循环线程） ---- */

    /** 取一个待发送 NAL；队列空返回 false。 */
    bool pop_nal(NalUnit& out);
    /** 记录当前活跃的 FramedSource（reuseFirstSource 下最多一个）。 */
    void attach_source(void* source);
    /** source 析构时解绑（仅当就是它本身时才清空）。 */
    void detach_source(void* source);
    /** 事件循环线程内取当前 source。 */
    [[nodiscard]] void* current_source() const;
    /** 取编码器缓存的参数集，用于填 SDP 的 sprop-parameter-sets。 */
    bool copy_sps_pps(std::vector<uint8_t>& sps, std::vector<uint8_t>& pps) const;
    /** 一帧的 RTP duration（微秒），给 DiscreteFramer 垫平节奏。 */
    [[nodiscard]] unsigned frame_duration_us() const;
    /** 事件循环线程（trigger 回调）：执行命令队列中的全部命令。 */
    void run_pending_cmds();
    /** 事件循环线程（trigger 回调）：把队列中的 NAL 交给当前 source。 */
    void deliver_pending_frames();

private:
    /** 投递给事件循环线程的命令。 */
    enum class Cmd : uint8_t {
        AddSession,     /**< 挂载 ServerMediaSession */
        RemoveSession,  /**< 断开客户端并撤下 session */
    };

    /**
     * 编码器 sink 回调：拆包入队并唤醒事件循环。
     * @param keyframe 本包是否含 IDR（积压恢复必须从关键帧接着传）。
     * @param pts_us 采帧时刻，本帧所有 NAL 共用（AU 边界判定依赖于此）。
     */
    bool on_encoded_packet(const uint8_t* annexb, size_t len, bool keyframe,
                           int64_t pts_us);
    /** 编码器摘除本路 sink 后的通知。 */
    void on_sink_dropped();
    /** 入队一条命令并触发事件循环处理。 */
    void post_cmd(Cmd cmd);
    /** 唤醒事件循环去取新入队的 NAL。 */
    void notify_frame_ready();
    /** live555 事件循环线程体。 */
    void loop_fn();
    /** 清空 NAL 队列（停流时）。 */
    void clear_queue();
    /** 距上次心跳满周期则打一行；调用前必须持有 mtx_。 */
    void maybe_log_stats_locked();
    /** 取首选局域网 IPv4（`lan_ipv4_list()` 首项）；无可用地址返回空串。 */
    static std::string local_ipv4();

    mutable std::mutex mtx_;              /**< 保护 NAL 队列、命令队列、会话状态。 */
    std::thread loop_thread_;             /**< live555 事件循环线程。 */
    char volatile loop_stop_ = 0;         /**< 置 1 使 doEventLoop 退出。 */
    bool loop_running_ = false;

    H264StreamEncoder* encoder_ = nullptr;
    int sink_id_ = -1;                    /**< 编码器 sink 句柄；<0 表示未注册。 */
    bool active_ = false;                 /**< 开关状态。 */
    uint16_t port_ = UPC_LOCAL_RTSP_PORT_DEFAULT;
    unsigned frame_duration_us_ = 33333;  /**< 默认 30fps；start() 时按编码器帧率改。 */

    std::deque<NalUnit> nal_queue_;
    uint32_t dropped_ = 0;                /**< 因无人拉流/积压/对齐 IDR 丢弃的包计数。 */
    uint32_t drop_events_ = 0;            /**< 触发「丢到下个 IDR」的次数。 */
    bool drop_until_idr_ = false;         /**< 积压或新客户端接入后，等到下一个 IDR 再送。 */
    std::deque<Cmd> cmd_queue_;

    /* 运行期心跳统计：队列深度持续贴近上限即说明 live555 取不赢，延迟会顶到
     * 队列容量对应的时长；in 涨而 out 不涨则说明事件循环卡住了。 */
    static constexpr int64_t kStatsPeriodUs = 5000000; /**< 心跳周期 5s。 */
    uint64_t stat_nal_in_ = 0;            /**< 累计入队 NAL 数。 */
    uint64_t stat_nal_out_ = 0;           /**< 累计被 source 取走的 NAL 数。 */
    int64_t  stat_last_log_us_ = 0;       /**< 上次心跳打印时刻；0=尚未打过。 */
    uint64_t stat_last_in_ = 0;           /**< 上次心跳时的 stat_nal_in_。 */
    uint64_t stat_last_out_ = 0;          /**< 上次心跳时的 stat_nal_out_。 */

    void* current_source_ = nullptr;      /**< H264FramedSource*，仅事件循环线程访问。 */
    /** 有无客户端在拉流。current_source_ 无锁且仅限事件循环线程，
     *  心跳日志跑在编码器 sink 线程上，故另用原子标志跨线程读。 */
    std::atomic<bool> has_client_{false};

    /* live555 对象用 void* 封装，避免头文件依赖 liveMedia.hh。 */
    void* scheduler_ = nullptr;           /**< BasicTaskScheduler* */
    void* env_ = nullptr;                 /**< BasicUsageEnvironment* */
    void* rtsp_server_ = nullptr;         /**< RTSPServer* */
    void* media_session_ = nullptr;       /**< ServerMediaSession*（挂载后非空） */
    uint32_t frame_trigger_ = 0;          /**< EventTriggerId：有新帧 */
    uint32_t cmd_trigger_ = 0;            /**< EventTriggerId：有新命令 */
};

} // namespace upc::media

#endif /* LOCAL_RTSP_SERVER_HPP */
