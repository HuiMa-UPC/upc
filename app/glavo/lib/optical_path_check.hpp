/**
 * @file optical_path_check.hpp
 * @brief 打印前光路检测：红光常亮逐点驻留，靠视觉确认红点后放行打印。
 *
 * =============================================================================
 * 在打印流程中的位置
 * =============================================================================
 * 优先级高于首层视觉检测。UI 已进入打印页（能看到相机实时画面），运动尚未开始：
 *
 *   launch_print_from_prepared
 *     ├─ print_running_ = true + 打印态通知
 *     ├─ 关舱灯 + 【本模块】光路检测 gate ──► 失败：报警 + 通知 UI 跳终止结果页
 *     ├─ 关舱灯 + 基板测高 gate（结束仍关灯，不开舱灯）
 *     ├─ open_file / start_parse
 *     ├─ 首层参考图 gate
 *     └─ begin_print_motion_after_reference_ready（此处才切激光模式）
 *
 * 单点流程：下发坐标 → settle 等待 → 请求视觉识别 → 等回包 → 通过则下一点。
 * 任一点超时未识别到即整轮失败（fail-closed），不重试、不放行。
 *
 * =============================================================================
 * 线程模型
 * =============================================================================
 * - start() 由 GlavoService worker 线程调用，内部起一条独立线程跑整轮。
 *   整轮最长约 5 × (settle + 3s)，绝不能占用 worker（那是唯一业务串行点）。
 * - on_point_rsp() 由 IPC 回调线程进入，配对 session_id + point_index 后唤醒等待。
 * - on_done 回调在检测线程上下文触发；GlavoService 只在其中置 pending + 唤醒
 *   worker，真正的起印动作仍回到 worker 串行执行。
 * - abort() 用于用户终止与进程退出，会打断 settle/等待并 join 线程。
 *
 * 依赖以回调注入（而非直接调 McuLink/MsgPort 单例），便于脱离硬件单测。
 */
#ifndef UPC_OPTICAL_PATH_CHECK_HPP
#define UPC_OPTICAL_PATH_CHECK_HPP

#include "galvo_motion_encode.hpp"
#include "upc_contract.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace upc::glavo {

/** 整轮检测结论。 */
struct OpticalPathCheckResult {
    bool     passed = false;
    /** 首个失败点位索引；passed 时为 kNoFailedPoint。 */
    uint8_t  failed_point_index = 0xff;
    /** 失败归因，仅进日志与告警文案：not_found / timeout / send_failed / aborted。 */
    const char* reason = "";
};

/** failed_point_index 的「无失败点」哨兵。 */
constexpr uint8_t kNoFailedPoint = 0xff;

class OpticalPathCheck {
public:
    /** 下发一个已编码的 11 字节运动子包；返回 false 表示链路拒发。 */
    using SendMotionFn = std::function<bool(const uint8_t* subpacket)>;
    /** 切红光常亮（检测期间）；返回 false 表示切换失败，整轮判失败。 */
    using ApplyRedAlwaysFn = std::function<bool()>;
    /** 向 Visual 发单点识别请求。 */
    using PublishCheckReqFn =
        std::function<bool(const middleware::VisualOpticalPathCheckReqPayload&)>;
    /** 逐点进度上报（→UI 打印卡徽标行）。 */
    using PublishProgressFn =
        std::function<void(const middleware::OpticalPathProgressPayload&)>;
    /** 会话开始/结束通知（→Visual 起流/释放）。 */
    using PublishSessionFn = std::function<void(bool active)>;
    /** 整轮结束回调（在检测线程上下文）。 */
    using DoneFn = std::function<void(const OpticalPathCheckResult&)>;

    OpticalPathCheck() = default;
    ~OpticalPathCheck();

    OpticalPathCheck(const OpticalPathCheck&) = delete;
    OpticalPathCheck& operator=(const OpticalPathCheck&) = delete;

    /** @brief 注入依赖。须在首次 start() 之前调用一次。 */
    void configure(SendMotionFn send_motion, ApplyRedAlwaysFn apply_red_always,
                   PublishCheckReqFn publish_check_req,
                   PublishProgressFn publish_progress,
                   PublishSessionFn publish_session, DoneFn on_done);

    /**
     * @brief 起一轮检测
     * @param cal 校准与增益快照（按值传入，规避 cal.csv 热加载竞争）
     * @return false=已有一轮在跑或依赖未注入，调用方不应等待 on_done
     */
    bool start(const GalvoMotionCalibration& cal);

    /**
     * @brief Visual 单点回包入口（IPC 回调线程）
     * @note session_id / point_index 不匹配的回包直接丢弃：3 秒窗口下
     *       上一点的迟到回包会污染下一点的判定。
     */
    void on_point_rsp(const middleware::VisualOpticalPathCheckRspPayload& rsp);

    /**
     * @brief 打断当前轮并回收线程（用户终止 / 进程退出）
     * @note 会递增 generation，被打断那轮的 on_done 不再触发。
     */
    void abort();

    [[nodiscard]] bool active() const noexcept { return active_.load(); }

    /** 本次打印会话是否已通过检测；用于避免异步回灌后重复起轮。 */
    [[nodiscard]] bool passed() const noexcept { return passed_.load(); }

    /** @brief 打印会话结束时清零，保证每次起印都重新检测。 */
    void reset_passed() noexcept { passed_.store(false); }

private:
    /** 单点判定结果，区分开来只为让告警文案能说清是"没找到"还是"视觉没回话"。 */
    enum class PointOutcome {
        Passed,   /**< 视觉回 result=1 */
        NotFound, /**< 视觉回 result=0，窗口内确实没找到红点 */
        Timeout,  /**< 视觉没回包，Glavo 兜底超时（fail-closed） */
        Aborted,  /**< 被 abort() 打断 */
    };

    void run(GalvoMotionCalibration cal, uint32_t generation);
    PointOutcome wait_point(uint8_t point_index, uint32_t generation);
    /** @return true=被打断（调用方应立即收尾）；false=正常睡满 */
    bool sleep_interruptible(std::chrono::milliseconds dur, uint32_t generation);
    void publish_progress(uint8_t point_index,
                          middleware::OpticalPathPointState state);
    /** generation 变化或 abort 置位都视为本轮作废。 */
    [[nodiscard]] bool round_cancelled(uint32_t generation) const noexcept;

    SendMotionFn       send_motion_;
    ApplyRedAlwaysFn   apply_red_always_;
    PublishCheckReqFn  publish_check_req_;
    PublishProgressFn  publish_progress_;
    PublishSessionFn   publish_session_;
    DoneFn             on_done_;

    std::thread       worker_;
    std::atomic<bool> active_{false};
    std::atomic<bool> passed_{false};
    std::atomic<bool> abort_requested_{false};
    /** 每轮自增；abort 后旧轮的回包与 on_done 一律作废。 */
    std::atomic<uint32_t> generation_{0};

    /** 保护下面这组等待状态，并与 settle/窗口等待共用一个条件变量。 */
    std::mutex              wait_mtx_;
    std::condition_variable wait_cv_;
    bool    rsp_pending_ = false;   /**< true=已发 Req 正在等回包 */
    bool    rsp_arrived_ = false;
    uint8_t rsp_result_ = 0;
    uint8_t rsp_point_index_ = 0xff;
};

} // namespace upc::glavo

#endif
