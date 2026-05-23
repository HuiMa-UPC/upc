#ifndef JOB_STATUS_H
#define JOB_STATUS_H

#include "ring_buffer.hpp"
#include <cstdint>
#include <mutex>
#include <cstring>

#ifdef __cplusplus
extern "C" {
#endif

/* 全局共享状态（UI线程通过lv_timer读取，后台线程写入） */

typedef struct {
    std::mutex mtx;

    /* 文件/解析 */
    char     current_file[256];
    int      total_lines;
    int      parsed_lines;
    int      total_packets;
    int      sent_packets;

    /* 环形缓冲（本地 g_ring_buffer，不依赖 TCP） */
    size_t   ring_used;       /* 当前包数 */
    size_t   ring_capacity;   /* 总容量 */
    bool     ring_full;       /* 队列已满，解析线程会在 put_wait 阻塞 */

    /* 下位机状态（从TCP应答解析） */
    uint16_t cmd_cache_rate;  /* 0..65535 */
    uint16_t data_cache_rate; /* 0..65535 */
    uint16_t current_x;
    uint16_t current_y;
    uint16_t current_z;
    uint16_t laser_state;
    uint8_t  flash_saved;     /* 0x0D脉冲 */

    /* 运行标志 */
    bool     is_parsing;
    bool     is_sending;
    bool     tcp_connected;
    bool     serial_connected;
    /** 下位机 Cmd 缓存过高：暂停运动帧下发，解析线程仍运行 */
    bool     mcu_cmd_tx_hold;

    /* 参数镜像（用于UI显示） */
    float    param_a_max;
    uint8_t  param_fly_enable;
    float    param_fly_gain;
    uint8_t  param_acc_enable;
    float    param_acc_delat;
    int32_t  param_laser_start;
    int32_t  param_laser_stop;
    bool     param_read_valid;   /* 已成功解析过读参应答 */
    bool     param_write_done; /* 写参后收到 Flash 保存脉冲(0x0D) */

    /* G-code 解析增益（Control 页设置，Files Open 重新解析时生效） */
    float    parse_image_scale_gain;
    float    parse_image_rang_gain;
    float    parse_speed_gain;

    /* 发送配置 */
    int      batch_size;
    int      interval_ms;
    bool     enable_layer_change;
    uint16_t layer_height_um;

} job_status_t;

extern job_status_t g_job_status;
extern RingBuffer   g_ring_buffer;

/** 从 g_ring_buffer 同步占用量到 g_job_status（UI 定时器 / 解析线程调用） */
void job_status_update_ring(void);

/** 根据 cmd_cache_rate 更新 mcu_cmd_tx_hold（须在已持有 JOB_LOCK 时调用） */
void job_status_sync_mcu_cmd_backpressure_locked(void);

/** 发送中是否应对解析线程节流（Cmd 背压 / 占用偏高 / Ring 接近满） */
bool job_status_parser_should_throttle(void);

/** 计算百分比并四舍五入（通用，也用于 MCU cache rate -> 0..100） */
int percent_rounded(int num, int den);

/* 辅助宏：上锁后安全读写 */
#define JOB_LOCK()    std::lock_guard<std::mutex> _job_lock(g_job_status.mtx)

#ifdef __cplusplus
}
#endif

#endif
