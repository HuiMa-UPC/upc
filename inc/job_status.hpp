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
    /** 本轮 G-code 已全部下发完毕，允许再次 Start */
    bool     job_print_completed;

    /* 参数镜像（用于UI显示） */
    float    param_a_max;
    uint8_t  param_fly_enable;
    float    param_fly_gain;
    uint8_t  param_acc_enable;
    float    param_acc_delat;
    int32_t  param_laser_start;
    int32_t  param_laser_stop;
    bool     param_read_valid;   /* 已成功解析过读参应答 */

    /* G-code 解析增益（Control 页设置，Files Open 重新解析时生效） */
    float    parse_image_scale_gain;
    float    parse_image_rang_gain;
    float    parse_speed_gain;

    /* 振镜校准（CAL Files 页 Open；Control 页使能后解析/下发生效） */
    char     cal_file[256];
    bool     cal_file_loaded;
    bool     enable_cal_apply;
    bool     cal_has_gains;
    float    cal_image_scale_gain;
    float    cal_image_rang_gain;
    float    cal_speed_gain;
    int      cal_grid_nx;
    int      cal_grid_ny;

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

/** 将 X/Y/Z 设为协议原点 offset（上电 / Reset 用） */
void job_status_reset_mcu_position(void);

/**
 * 应用下位机 19B 坐标：各轴为 0 时不覆盖（下位机空闲常报 0，非有效 offset）
 * 调用方已持有 JOB_LOCK
 */
void job_status_apply_mcu_position_locked(uint16_t x, uint16_t y, uint16_t z);

/** MCU 队列占用标签（小百分比保留两位小数） */
void format_mcu_queue_rate_label(char *buf, size_t buf_size, uint16_t rate);

/** 根据 cmd_cache_rate 更新 mcu_cmd_tx_hold（须在已持有 JOB_LOCK 时调用） */
void job_status_sync_mcu_cmd_backpressure_locked(void);

/** 计算百分比并四舍五入（通用，也用于 MCU cache rate -> 0..100） */
int percent_rounded(int num, int den);

/** 计算百分比 ×10 并四舍五入（用于 MCU rate 一位小数显示，1000 = 100.0%） */
int percent_tenths(int num, int den);

/** 下位机 19B 坐标 offset：G-code 逻辑 0 对应 raw 32768（Reset 时刷到该值） */
#define MCU_COORD_ZERO_OFFSET 32768

/* 辅助宏：上锁后安全读写 */
#define JOB_LOCK()    std::lock_guard<std::mutex> _job_lock(g_job_status.mtx)

#ifdef __cplusplus
}
#endif

#endif
