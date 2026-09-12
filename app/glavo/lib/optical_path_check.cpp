/**
 * @file optical_path_check.cpp
 * @brief 打印前光路检测状态机实现。
 *
 * 时序常量的取值理由见各自注释；判定一律 fail-closed —— 拿不到肯定答复就当光路异常，
 * 宁可误挡一次打印，也不能在光路真有问题时放激光出去。
 */
#include "optical_path_check.hpp"

#include "rs_log.hpp"

namespace upc::glavo {

namespace {

/**
 * 红光下发到实际稳定在新点位之间的等待（TCP 传输 + MCU 执行 + 相机曝光收敛）。
 * 不留这段，判定窗口的前几帧还停在上一个点位上，会把「上一点仍亮」误判成本点通过。
 */
constexpr auto kPointSettle = std::chrono::milliseconds(250);

/** 单点判定窗口：视觉在此期间反复取帧识别，与需求约定的 3 秒一致。 */
constexpr uint32_t kPointWindowMs = 3000;

/**
 * 整轮开始前的一次性取流预热。
 * 会话通知发出后 Visual 还要走完「请求 Camera 起流 → 分配 dma-buf → 回执 →
 * mmap → 拉起取帧线程 → 攒出第一帧」，冷启动下这条链路要几百毫秒到一秒多。
 * 不预热就等于让第 0 点用自己的 3 秒窗口替相机买单，链路慢一点就会把好光路
 * 误判成异常 —— 这类误报比多等一秒的代价大得多。
 */
constexpr auto kStreamWarmup = std::chrono::milliseconds(1500);

/**
 * Glavo 侧兜底等待相对判定窗口的余量：覆盖 IPC 往返与视觉收尾。
 * 视觉进程挂掉/回包丢失时靠这条超时收敛，按未通过处理。
 */
constexpr auto kPointReplyGrace = std::chrono::milliseconds(700);

} // namespace

OpticalPathCheck::~OpticalPathCheck()
{
    abort();
}

void OpticalPathCheck::configure(SendMotionFn send_motion,
                                 ApplyRedAlwaysFn apply_red_always,
                                 PublishCheckReqFn publish_check_req,
                                 PublishProgressFn publish_progress,
                                 PublishSessionFn publish_session,
                                 DoneFn on_done)
{
    send_motion_ = std::move(send_motion);
    apply_red_always_ = std::move(apply_red_always);
    publish_check_req_ = std::move(publish_check_req);
    publish_progress_ = std::move(publish_progress);
    publish_session_ = std::move(publish_session);
    on_done_ = std::move(on_done);
}

bool OpticalPathCheck::round_cancelled(uint32_t generation) const noexcept
{
    return abort_requested_.load() || generation_.load() != generation;
}

bool OpticalPathCheck::start(const GalvoMotionCalibration& cal)
{
    if(!send_motion_ || !apply_red_always_ || !publish_check_req_ || !on_done_) {
        UPC_LOG_GLAVO_SERVICE(Error, "optical path start rejected: not configured");
        return false;
    }

    bool expected = false;
    if(!active_.compare_exchange_strong(expected, true)) {
        UPC_LOG_GLAVO_SERVICE(Warn, "optical path start ignored: already active");
        return false;
    }

    /* 上一轮线程可能刚跑完还没回收；此处 join 不会阻塞，因为 active_ 已经落回 false。 */
    if(worker_.joinable()) {
        worker_.join();
    }

    abort_requested_.store(false);
    const uint32_t generation = generation_.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(wait_mtx_);
        rsp_pending_ = false;
        rsp_arrived_ = false;
        rsp_result_ = 0;
        rsp_point_index_ = kNoFailedPoint;
    }

    GalvoMotionCalibration snapshot = cal;
    worker_ = std::thread([this, snapshot, generation]() {
        run(snapshot, generation);
    });
    return true;
}

void OpticalPathCheck::abort()
{
    /* 先递增 generation：正在跑的那轮从此刻起所有判定与 on_done 都作废。 */
    generation_.fetch_add(1);
    abort_requested_.store(true);
    wait_cv_.notify_all();
    if(worker_.joinable()) {
        worker_.join();
    }
    active_.store(false);
}

void OpticalPathCheck::publish_progress(uint8_t point_index,
                                        middleware::OpticalPathPointState state)
{
    if(!publish_progress_) {
        return;
    }
    middleware::OpticalPathProgressPayload p{};
    p.point_index = point_index;
    p.point_total = static_cast<uint8_t>(kGalvoTestPoints.size());
    p.state = static_cast<uint8_t>(state);
    publish_progress_(p);
}

bool OpticalPathCheck::sleep_interruptible(std::chrono::milliseconds dur,
                                           uint32_t generation)
{
    std::unique_lock<std::mutex> lock(wait_mtx_);
    return wait_cv_.wait_for(lock, dur, [this, generation]() {
        return abort_requested_.load() || generation_.load() != generation;
    });
}

OpticalPathCheck::PointOutcome OpticalPathCheck::wait_point(uint8_t point_index,
                                                            uint32_t generation)
{
    {
        std::lock_guard<std::mutex> lock(wait_mtx_);
        rsp_pending_ = true;
        rsp_arrived_ = false;
        rsp_result_ = 0;
        rsp_point_index_ = point_index;
    }

    const auto& pt = kGalvoTestPoints[point_index];
    middleware::VisualOpticalPathCheckReqPayload req{};
    req.point_index = point_index;
    req.point_total = static_cast<uint8_t>(kGalvoTestPoints.size());
    req.expect_x_mm_x10 = static_cast<int16_t>(pt.x * 10.0f);
    req.expect_y_mm_x10 = static_cast<int16_t>(pt.y * 10.0f);
    req.timeout_ms = kPointWindowMs;
    req.session_id = generation;
    if(!publish_check_req_(req)) {
        std::lock_guard<std::mutex> lock(wait_mtx_);
        rsp_pending_ = false;
        UPC_LOG_GLAVO_SERVICE(Warn,
            "optical path check req publish failed point=%u",
            static_cast<unsigned>(point_index));
        return PointOutcome::Timeout;
    }

    const auto budget = std::chrono::milliseconds(kPointWindowMs) + kPointReplyGrace;
    std::unique_lock<std::mutex> lock(wait_mtx_);
    (void)wait_cv_.wait_for(lock, budget, [this, generation]() {
        return rsp_arrived_ || abort_requested_.load() ||
               generation_.load() != generation;
    });
    rsp_pending_ = false;

    /* 先判 abort：打断与超时都会让 wait_for 返回，但两者归因不同。
     * 排除 abort 后 rsp_arrived_ 就是唯一判据（超时时它必为 false）。 */
    if(abort_requested_.load() || generation_.load() != generation) {
        return PointOutcome::Aborted;
    }
    if(!rsp_arrived_) {
        return PointOutcome::Timeout;
    }
    return (rsp_result_ != 0) ? PointOutcome::Passed : PointOutcome::NotFound;
}

void OpticalPathCheck::on_point_rsp(
    const middleware::VisualOpticalPathCheckRspPayload& rsp)
{
    std::lock_guard<std::mutex> lock(wait_mtx_);
    /* 会话号或点位不匹配：上一点的迟到回包，丢弃以免污染当前点判定。 */
    if(!rsp_pending_ || rsp.session_id != generation_.load() ||
       rsp.point_index != rsp_point_index_) {
        UPC_LOG_GLAVO_SERVICE(Warn,
            "optical path rsp dropped pending=%d session=%u point=%u result=%u",
            rsp_pending_ ? 1 : 0,
            static_cast<unsigned>(rsp.session_id),
            static_cast<unsigned>(rsp.point_index),
            static_cast<unsigned>(rsp.result));
        return;
    }
    rsp_arrived_ = true;
    rsp_result_ = rsp.result ? 1u : 0u;
    UPC_LOG_GLAVO_SERVICE(Info,
        "optical path rsp point=%u result=%u conf=%u px=%d,%d",
        static_cast<unsigned>(rsp.point_index),
        static_cast<unsigned>(rsp.result),
        static_cast<unsigned>(rsp.confidence_q8),
        static_cast<int>(rsp.detected_x_px),
        static_cast<int>(rsp.detected_y_px));
    wait_cv_.notify_all();
}

void OpticalPathCheck::run(GalvoMotionCalibration cal, uint32_t generation)
{
    if(publish_session_) {
        publish_session_(true);
    }

    OpticalPathCheckResult result;
    result.failed_point_index = kNoFailedPoint;
    result.reason = "";
    bool ok = true;

    if(!apply_red_always_()) {
        ok = false;
        result.reason = "red_always_failed";
        result.failed_point_index = 0;
        UPC_LOG_GLAVO_SERVICE(Warn, "optical path aborted: red-always switch failed");
    }

    /* 等 Visual 那一路取流就绪再开第一个判定窗口（理由见 kStreamWarmup）。
     * 放在红光切换之后：切换失败时不必白等这一秒半。 */
    if(ok && sleep_interruptible(kStreamWarmup, generation)) {
        ok = false;
        result.reason = "aborted";
    }

    const auto point_count = static_cast<uint8_t>(kGalvoTestPoints.size());
    for(uint8_t i = 0; ok && i < point_count; ++i) {
        if(round_cancelled(generation)) {
            ok = false;
            result.reason = "aborted";
            break;
        }

        const auto& pt = kGalvoTestPoints[i];
        const auto packet = galvo_encode_motion_subpacket(cal, pt.x, pt.y);
        if(!send_motion_(packet.data())) {
            ok = false;
            result.reason = "send_failed";
            result.failed_point_index = i;
            UPC_LOG_GLAVO_SERVICE(Warn,
                "optical path move rejected point=%u x=%.1f y=%.1f",
                static_cast<unsigned>(i),
                static_cast<double>(pt.x), static_cast<double>(pt.y));
            break;
        }
        UPC_LOG_GLAVO_SERVICE(Info,
            "optical path move point=%u x=%.1f y=%.1f",
            static_cast<unsigned>(i),
            static_cast<double>(pt.x), static_cast<double>(pt.y));
        publish_progress(i, middleware::OpticalPathPointState::Moving);

        if(sleep_interruptible(kPointSettle, generation)) {
            ok = false;
            result.reason = "aborted";
            break;
        }

        const PointOutcome outcome = wait_point(i, generation);
        if(outcome == PointOutcome::Passed) {
            publish_progress(i, middleware::OpticalPathPointState::Passed);
            continue;
        }

        ok = false;
        result.failed_point_index = i;
        switch(outcome) {
        case PointOutcome::NotFound: result.reason = "not_found"; break;
        case PointOutcome::Timeout:  result.reason = "timeout";   break;
        case PointOutcome::Aborted:  result.reason = "aborted";   break;
        case PointOutcome::Passed:   break; /* 上面已 continue */
        }
        if(outcome != PointOutcome::Aborted) {
            publish_progress(i, middleware::OpticalPathPointState::Failed);
            UPC_LOG_GLAVO_SERVICE(Warn,
                "optical path failed point=%u reason=%s",
                static_cast<unsigned>(i), result.reason);
        }
    }

    if(publish_session_) {
        publish_session_(false);
    }

    result.passed = ok;
    if(ok) {
        passed_.store(true);
        publish_progress(0, middleware::OpticalPathPointState::Complete);
        UPC_LOG_GLAVO_SERVICE(Info, "optical path passed points=%u",
                              static_cast<unsigned>(point_count));
    }
    active_.store(false);

    /* 被 abort 的那轮不回调：调用方此刻已在自己的终止路径上，
     * 再回一次 on_done 会与它抢同一份打印会话状态。 */
    if(round_cancelled(generation)) {
        UPC_LOG_GLAVO_SERVICE(Info, "optical path round cancelled gen=%u",
                              static_cast<unsigned>(generation));
        return;
    }
    on_done_(result);
}

} // namespace upc::glavo
