#include "job_status.hpp"
#include "pipeline_tune.hpp"

#include <cstdio>

job_status_t g_job_status;
RingBuffer   g_ring_buffer;

void job_status_reset_mcu_position(void)
{
    JOB_LOCK();
    g_job_status.current_x = MCU_COORD_ZERO_OFFSET;
    g_job_status.current_y = MCU_COORD_ZERO_OFFSET;
    g_job_status.current_z = MCU_COORD_ZERO_OFFSET;
}

void job_status_apply_mcu_position_locked(uint16_t x, uint16_t y, uint16_t z)
{
    if(x != 0) {
        g_job_status.current_x = x;
    }
    if(y != 0) {
        g_job_status.current_y = y;
    }
    if(z != 0) {
        g_job_status.current_z = z;
    }
}

void format_mcu_queue_rate_label(char *buf, size_t buf_size, uint16_t rate)
{
    if(!buf || buf_size == 0) {
        return;
    }
    const double pct = (double)rate * 100.0 / (double)MCU_CACHE_RATE_MAX;
    if(pct >= 100.0) {
        snprintf(buf, buf_size, "100%%");
    } else if(pct >= 10.0) {
        snprintf(buf, buf_size, "%.1f%%", pct);
    } else if(pct >= 0.005) {
        snprintf(buf, buf_size, "%.2f%%", pct);
    } else {
        snprintf(buf, buf_size, "0%%");
    }
}

static bool mcu_cmd_rate_at_least_pct(uint16_t rate, int pct)
{
    return (uint32_t)rate * 100U >= (uint32_t)MCU_CACHE_RATE_MAX * (uint32_t)pct;
}

static bool mcu_cmd_rate_overloaded(uint16_t rate)
{
    return mcu_cmd_rate_at_least_pct(rate, MCU_CMD_BACKPRESSURE_ON_PCT);
}

static bool mcu_cmd_rate_below_resume(uint16_t rate)
{
    return !mcu_cmd_rate_at_least_pct(rate, MCU_CMD_BACKPRESSURE_OFF_PCT);
}

void job_status_update_ring(void)
{
    const size_t used = g_ring_buffer.count();
    const size_t cap  = g_ring_buffer.capacity();

    JOB_LOCK();
    g_job_status.ring_used     = used;
    g_job_status.ring_capacity = cap;
    const unsigned pause_pct = g_job_status.is_sending
                                   ? (unsigned)RB_PRODUCER_PAUSE_PCT
                                   : (unsigned)RB_PRELOAD_PAUSE_PCT;
    const size_t pause_pkts = (cap * pause_pct) / 100U;
    g_job_status.ring_full     = (cap > 0 && used >= pause_pkts);
}

void job_status_sync_mcu_cmd_backpressure_locked(void)
{
    if(!g_job_status.is_sending || !g_job_status.tcp_connected) {
        g_job_status.mcu_cmd_tx_hold = false;
        return;
    }

    const uint16_t cmd = g_job_status.cmd_cache_rate;
    if(g_job_status.mcu_cmd_tx_hold) {
        if(mcu_cmd_rate_below_resume(cmd)) {
            g_job_status.mcu_cmd_tx_hold = false;
        }
    } else if(cmd > 0 && mcu_cmd_rate_overloaded(cmd)) {
        g_job_status.mcu_cmd_tx_hold = true;
    }
}

int percent_rounded(int num, int den)
{
    if(den <= 0) {
        return 0;
    }
    if(num >= den) {
        return 100;
    }
    return (num * 100 + den / 2) / den;
}

int percent_tenths(int num, int den)
{
    if(den <= 0) {
        return 0;
    }
    if(num >= den) {
        return 1000;
    }
    return (num * 1000 + den / 2) / den;
}
