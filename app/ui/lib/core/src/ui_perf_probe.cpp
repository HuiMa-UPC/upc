#include "ui_perf_probe.hpp"

#include "rs_log.hpp"

#if UPC_LOG_ENABLED && UPC_MOD_CAMERA_PREVIEW

#include <chrono>

namespace ui {

namespace {

constexpr uint64_t kReportPeriodUs = 5ull * 1000ull * 1000ull;

/** 一段耗时的「累计 + 峰值」，输出后清零。 */
struct Acc {
    uint64_t sum = 0;
    uint64_t max = 0;

    void add(uint64_t v)
    {
        sum += v;
        if(v > max) {
            max = v;
        }
    }
    uint64_t avg(uint32_t n) const { return n > 0 ? sum / n : 0; }
    void reset()
    {
        sum = 0;
        max = 0;
    }
};

/* ============================================================
 * 主循环分段
 * ============================================================ */
struct LoopStats {
    uint32_t loops = 0;
    Acc      ipc_wait;
    Acc      lv_timer;
    Acc      ipc_flush;
    Acc      monitor;
    uint64_t report_at_us = 0; /**< 0 = 尚未开始计时，首次调用时对齐 */
};
LoopStats g_loop;

/* ============================================================
 * LVGL 刷新分段
 *
 * 一次刷新里 LVGL 发事件的顺序（lvgl/src/core/lv_refr.c）：
 *   REFR_START
 *     lv_obj_update_layout ×5 / lv_refr_join_area / refr_sync_areas
 *     RENDER_START
 *       [每个失效区一轮] FLUSH_WAIT_START/FINISH → FLUSH_START/FINISH
 *     RENDER_READY
 *   REFR_READY
 *
 * 由此得到一个可加的分解：refr = pre + draw + wait + flush
 *   pre   = refr - render：布局更新 + 双缓冲同步区拷贝（DIRECT 双缓冲下
 *           refr_sync_areas 要把上一帧的失效区从在屏缓冲拷到离屏缓冲，
 *           整屏预览时这是一次全屏 memcpy，不容忽视）
 *   draw  = render - wait - flush：纯绘制
 *   wait  = 等 DRM 翻页（drm_flush_wait 里的 poll(-1)）
 *   flush = flush_cb 本身（drmModeAtomicCommit）
 *
 * DIRECT 模式下失效区可能不止一个，而每个区都要走一遍 wait_for_flushing，
 * 所以 `waits` 除以 `n` 能看出一次刷新等了几回翻页——若这个比值明显大于 1，
 * 说明失效区没合并，每帧被多个 vblank 串行拖住。
 * ============================================================ */
struct RefrStats {
    uint32_t n       = 0;
    uint32_t idle    = 0; /**< 进了刷新但没有失效区可画的次数 */
    uint32_t waits   = 0;
    uint32_t flushes = 0;
    Acc      refr;
    Acc      pre;
    Acc      draw;
    Acc      wait;
    Acc      flush;
    uint64_t report_at_us = 0;
};
RefrStats g_refr;

/**
 * 当前这一次刷新的临时量。
 * 起始时刻用 0 当「未开始」哨兵：perf_now_us() 取的是 CLOCK_MONOTONIC 微秒
 * （开机至今），实际不可能为 0。
 */
struct CurRefr {
    uint64_t refr_start_us   = 0;
    uint64_t render_start_us = 0;
    uint64_t wait_start_us   = 0;
    uint64_t flush_start_us  = 0;
    uint64_t render_us       = 0;
    uint64_t wait_us         = 0;
    uint64_t flush_us        = 0;
    uint32_t waits           = 0;
    uint32_t flushes         = 0;
};
CurRefr g_cur;

/** 差值截断到 0：分解出来的分段相减理论上非负，但别让抖动写出天文数字。 */
uint64_t sub_sat(uint64_t a, uint64_t b)
{
    return a > b ? a - b : 0;
}

void on_refr_start(lv_event_t*)
{
    g_cur                = CurRefr{}; /* 每次刷新重新开始累计 */
    g_cur.refr_start_us  = perf_now_us();
}

void on_render_start(lv_event_t*)
{
    g_cur.render_start_us = perf_now_us();
}

void on_render_ready(lv_event_t*)
{
    if(g_cur.render_start_us == 0) {
        return;
    }
    g_cur.render_us = perf_now_us() - g_cur.render_start_us;
}

void on_wait_start(lv_event_t*)
{
    g_cur.wait_start_us = perf_now_us();
}

void on_wait_finish(lv_event_t*)
{
    if(g_cur.wait_start_us == 0) {
        return;
    }
    g_cur.wait_us += perf_now_us() - g_cur.wait_start_us;
    g_cur.wait_start_us = 0;
    ++g_cur.waits;
}

void on_flush_start(lv_event_t*)
{
    g_cur.flush_start_us = perf_now_us();
}

void on_flush_finish(lv_event_t*)
{
    if(g_cur.flush_start_us == 0) {
        return;
    }
    g_cur.flush_us += perf_now_us() - g_cur.flush_start_us;
    g_cur.flush_start_us = 0;
    ++g_cur.flushes;
}

void on_refr_ready(lv_event_t*)
{
    const uint64_t now = perf_now_us();
    if(g_cur.refr_start_us == 0) {
        return; /* 没收到配对的 REFR_START，丢弃这个样本 */
    }
    const uint64_t refr_us = now - g_cur.refr_start_us;

    if(g_refr.report_at_us == 0) {
        g_refr.report_at_us = now + kReportPeriodUs;
    }

    if(g_cur.render_us == 0) {
        /* 定时器跑到了但没有失效区（或没画成）：不进平均，单独计数。 */
        ++g_refr.idle;
    }
    else {
        ++g_refr.n;
        g_refr.waits += g_cur.waits;
        g_refr.flushes += g_cur.flushes;
        g_refr.refr.add(refr_us);
        g_refr.pre.add(sub_sat(refr_us, g_cur.render_us));
        g_refr.draw.add(
            sub_sat(g_cur.render_us, g_cur.wait_us + g_cur.flush_us));
        g_refr.wait.add(g_cur.wait_us);
        g_refr.flush.add(g_cur.flush_us);
    }
    g_cur.refr_start_us = 0;

    if(now < g_refr.report_at_us) {
        return;
    }
    const uint32_t n = g_refr.n;
    UPC_LOG_CAMERA_PREVIEW(Debug,
        "refr 5s: n=%u idle=%u refr_avg_us=%llu refr_max_us=%llu "
        "pre_avg_us=%llu pre_max_us=%llu "
        "draw_avg_us=%llu draw_max_us=%llu "
        "wait_avg_us=%llu wait_max_us=%llu waits=%u "
        "flush_avg_us=%llu flush_max_us=%llu flushes=%u",
        n, g_refr.idle,
        static_cast<unsigned long long>(g_refr.refr.avg(n)),
        static_cast<unsigned long long>(g_refr.refr.max),
        static_cast<unsigned long long>(g_refr.pre.avg(n)),
        static_cast<unsigned long long>(g_refr.pre.max),
        static_cast<unsigned long long>(g_refr.draw.avg(n)),
        static_cast<unsigned long long>(g_refr.draw.max),
        static_cast<unsigned long long>(g_refr.wait.avg(n)),
        static_cast<unsigned long long>(g_refr.wait.max),
        g_refr.waits,
        static_cast<unsigned long long>(g_refr.flush.avg(n)),
        static_cast<unsigned long long>(g_refr.flush.max),
        g_refr.flushes);

    g_refr.n = g_refr.idle = g_refr.waits = g_refr.flushes = 0;
    g_refr.refr.reset();
    g_refr.pre.reset();
    g_refr.draw.reset();
    g_refr.wait.reset();
    g_refr.flush.reset();
    g_refr.report_at_us = now + kReportPeriodUs;
}

} // namespace

uint64_t perf_now_us()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void perf_loop_sample(uint64_t ipc_wait_us, uint64_t lv_timer_us,
                      uint64_t ipc_flush_us, uint64_t monitor_us)
{
    ++g_loop.loops;
    g_loop.ipc_wait.add(ipc_wait_us);
    g_loop.lv_timer.add(lv_timer_us);
    g_loop.ipc_flush.add(ipc_flush_us);
    g_loop.monitor.add(monitor_us);

    const uint64_t now = perf_now_us();
    if(g_loop.report_at_us == 0) {
        g_loop.report_at_us = now + kReportPeriodUs;
        return;
    }
    if(now < g_loop.report_at_us) {
        return;
    }

    const uint32_t n = g_loop.loops;
    UPC_LOG_CAMERA_PREVIEW(Debug,
        "uiloop 5s: loops=%u "
        "ipcwait_avg_us=%llu ipcwait_max_us=%llu "
        "lv_avg_us=%llu lv_max_us=%llu "
        "ipc_avg_us=%llu ipc_max_us=%llu "
        "mon_avg_us=%llu mon_max_us=%llu",
        n,
        static_cast<unsigned long long>(g_loop.ipc_wait.avg(n)),
        static_cast<unsigned long long>(g_loop.ipc_wait.max),
        static_cast<unsigned long long>(g_loop.lv_timer.avg(n)),
        static_cast<unsigned long long>(g_loop.lv_timer.max),
        static_cast<unsigned long long>(g_loop.ipc_flush.avg(n)),
        static_cast<unsigned long long>(g_loop.ipc_flush.max),
        static_cast<unsigned long long>(g_loop.monitor.avg(n)),
        static_cast<unsigned long long>(g_loop.monitor.max));

    g_loop.loops = 0;
    g_loop.ipc_wait.reset();
    g_loop.lv_timer.reset();
    g_loop.ipc_flush.reset();
    g_loop.monitor.reset();
    g_loop.report_at_us = now + kReportPeriodUs;
}

void perf_attach_display(lv_display_t* disp)
{
    if(!disp) {
        UPC_LOG_CAMERA_PREVIEW(Warn, "perf probe: no display, refr 5s disabled");
        return;
    }
    lv_display_add_event_cb(disp, on_refr_start, LV_EVENT_REFR_START, nullptr);
    lv_display_add_event_cb(disp, on_render_start, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(disp, on_render_ready, LV_EVENT_RENDER_READY, nullptr);
    lv_display_add_event_cb(disp, on_wait_start, LV_EVENT_FLUSH_WAIT_START, nullptr);
    lv_display_add_event_cb(disp, on_wait_finish, LV_EVENT_FLUSH_WAIT_FINISH, nullptr);
    lv_display_add_event_cb(disp, on_flush_start, LV_EVENT_FLUSH_START, nullptr);
    lv_display_add_event_cb(disp, on_flush_finish, LV_EVENT_FLUSH_FINISH, nullptr);
    /* REFR_READY 必须最后挂：它负责汇总本次刷新并输出 5s 聚合。 */
    lv_display_add_event_cb(disp, on_refr_ready, LV_EVENT_REFR_READY, nullptr);
    UPC_LOG_CAMERA_PREVIEW(Info,
        "perf probe attached res=%dx%d rotation=%d refr_period=%d",
        static_cast<int>(lv_display_get_horizontal_resolution(disp)),
        static_cast<int>(lv_display_get_vertical_resolution(disp)),
        static_cast<int>(lv_display_get_rotation(disp)),
        static_cast<int>(LV_DEF_REFR_PERIOD));
}

} // namespace ui

#else /* !(UPC_LOG_ENABLED && UPC_MOD_CAMERA_PREVIEW) */

namespace ui {

uint64_t perf_now_us()
{
    return 0;
}

void perf_loop_sample(uint64_t, uint64_t, uint64_t, uint64_t) {}

void perf_attach_display(lv_display_t*) {}

} // namespace ui

#endif /* UPC_LOG_ENABLED && UPC_MOD_CAMERA_PREVIEW */
