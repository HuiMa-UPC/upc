#include "job_status.hpp"
#include "pipeline_tune.hpp"
#include "gcode_parse_gains.hpp"

job_status_t g_job_status;
RingBuffer   g_ring_buffer;

void gcode_parse_gains_set_defaults(void)
{
    JOB_LOCK();
    g_job_status.parse_image_scale_gain = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
    g_job_status.parse_image_rang_gain  = GCODE_DEFAULT_IMAGE_RANG_GAIN;
    g_job_status.parse_speed_gain       = GCODE_DEFAULT_SPEED_GAIN;
}

static bool mcu_cmd_rate_at_least_pct(uint16_t rate, int pct)
{
    return (uint32_t)rate * 100U >= (uint32_t)MCU_CACHE_RATE_MAX * (uint32_t)pct;
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
    /* 尚未收到复位后第一条心跳前不做 Cmd 背压（避免沿用复位前的陈旧占用率） */
    if(cmd == 0) {
        g_job_status.mcu_cmd_tx_hold = false;
        return;
    }
    if(g_job_status.mcu_cmd_tx_hold) {
        if(!mcu_cmd_rate_at_least_pct(cmd, MCU_CMD_BACKPRESSURE_OFF_PCT)) {
            g_job_status.mcu_cmd_tx_hold = false;
        }
    } else if(mcu_cmd_rate_at_least_pct(cmd, MCU_CMD_BACKPRESSURE_ON_PCT)) {
        g_job_status.mcu_cmd_tx_hold = true;
    }
}

bool job_status_parser_should_throttle(void)
{
    bool throttle = false;
    JOB_LOCK();
    if(!g_job_status.is_sending) {
        throttle = g_job_status.ring_full;
    } else if(g_job_status.mcu_cmd_tx_hold) {
        throttle = true;
    } else if(g_job_status.cmd_cache_rate > 0 &&
              mcu_cmd_rate_at_least_pct(g_job_status.cmd_cache_rate,
                                        MCU_CMD_BACKPRESSURE_OFF_PCT)) {
        /* 未到 90% 停发前，解析先减速，减轻下位机压力 */
        throttle = true;
    } else if(g_job_status.ring_capacity > 0) {
        const size_t used = g_job_status.ring_used;
        const size_t cap  = g_job_status.ring_capacity;
        throttle = (used * 100U >= cap * (unsigned)GCODE_PARSE_RING_SLOW_PCT);
    }
    return throttle;
}
