#ifndef JOB_STATUS_H
#define JOB_STATUS_H

/**
 * @file job_status.hpp
 * @brief 全局作业状态中心（UI 与各后台线程之间的共享数据）
 *
 * 作用：
 *   - 定义 `g_job_status`：当前 G-code 作业、解析进度、Ring 水位、TCP/串口连接、
 *     下位机反馈、板参数、解析增益、校准、发送配置等，供多模块读写。
 *   - 定义 `g_ring_buffer`：解析后的运动包队列（与 g_job_status 同文件导出，便于绑定）。
 *   - 提供线程安全辅助：`JOB_LOCK()`、Ring 同步、MCU 背压、坐标与百分比工具函数。
 *
 * 线程模型：
 *   - UI 定时器（Home/Control/Files）在 JOB_LOCK 下读字段刷新界面。
 *   - 解析线程（gcode_worker）、链路线程（ctrl_backend）在 JOB_LOCK 下写进度/连接/下位机状态。
 *   - 写后 UI 下一轮 timer 即可看到更新；勿在持锁时调用 LVGL API。
 *   - 禁止在 JOB_LOCK 作用域内再调用 JOB_LOCK()（mutex 不可重入，同线程会死锁）。
 *     典型陷阱：持锁时调用 page_files_notify_parse_restarted() / refresh_detail_panel()。
 *   - job_status_update_ring() 会先锁 Ring 再锁 job_status；勿在已持有 JOB_LOCK 时调用。
 *
 * 引用本头的模块见各 .cpp 顶部的 #include "job_status.hpp"。
 */

#include "ring_buffer.hpp"
#include <cstdint>
#include <mutex>
#include <cstring>

#ifdef __cplusplus
extern "C" {
#endif

/** 全局共享状态（UI 线程读、后台线程写，访问字段须 JOB_LOCK） */
typedef struct {
    std::mutex mtx;

    /* ---------- 文件 / 解析进度 ---------- */
    char     current_file[256];  /**< 当前 G-code 文件名（basename） */
    int      total_lines;        /**< 文件总行数（Open 时预统计） */
    int      parsed_lines;       /**< 已解析行数 */
    int      total_packets;      /**< 已生成运动包数（不含 M3000 换层标记） */
    int      sent_packets;       /**< 已通过 TCP 下发的运动包数 */

    /* ---------- RingBuffer 镜像（数据源为 g_ring_buffer） ---------- */
    size_t   ring_used;          /**< 当前队列包数 */
    size_t   ring_capacity;      /**< 队列容量（RB_MAX_PACKETS） */
    bool     ring_full;          /**< 达到预填暂停阈值，解析线程在 put_wait 阻塞 */

    /* ---------- 下位机 TCP 常态应答 19B ---------- */
    uint16_t cmd_cache_rate;     /**< Cmd 队列占用 0..65535 */
    uint16_t data_cache_rate;    /**< Data 队列占用 0..65535 */
    uint16_t current_x;          /**< 振镜 X raw（0 表示不更新） */
    uint16_t current_y;
    uint16_t current_z;
    uint16_t laser_state;
    uint8_t  flash_saved;        /**< 写参后 Flash 保存脉冲 0x0D，UI 读后清零 */

    /* ---------- 运行标志 ---------- */
    bool     is_parsing;         /**< 解析线程运行中 */
    bool     is_sending;         /**< 正在向下位机下发运动流 */
    bool     tcp_connected;
    bool     serial_connected;
    bool     mcu_cmd_tx_hold;    /**< Cmd 缓存过高，暂停 TCP 运动下发 */
    bool     job_print_completed;/**< 本轮打印已全部完成，可再次 Start */

    /* ---------- 板参数镜像（Settings 读/写、Control 显示） ---------- */
    float    param_a_max;
    uint8_t  param_fly_enable;
    float    param_fly_gain;
    uint8_t  param_acc_enable;
    float    param_acc_delat;
    int32_t  param_laser_start;
    int32_t  param_laser_stop;
    bool     param_read_valid;

    /* ---------- G-code 解析增益（Control 设置，解析线程读取） ---------- */
    float    parse_image_scale_gain;
    float    parse_image_rang_gain;
    float    parse_speed_gain;

    /** 振镜打标中心点手动偏移（G-code 平面，mm；无掉电记忆） */
    float    center_offset_x;
    float    center_offset_y;

    /* ---------- 振镜校准（CAL Files Open，Control 使能后解析生效） ---------- */
    char     cal_file[256];
    bool     cal_file_loaded;
    bool     enable_cal_apply;
    bool     cal_has_gains;
    float    cal_image_scale_gain;
    float    cal_image_rang_gain;
    float    cal_speed_gain;
    int      cal_grid_nx;
    int      cal_grid_ny;

    /* ---------- 发送配置（Control Batch/Intv/换层） ---------- */
    int      batch_size;
    int      interval_ms;
    bool     enable_layer_change;
    uint16_t layer_height_um;    /**< 换层串口帧层厚 μm */

} job_status_t;

/** 全局作业状态（全工程唯一实例） */
extern job_status_t g_job_status;

/** 全局解析队列（Files Open 后 gcode_worker 写入，ctrl_backend 消费） */
extern RingBuffer   g_ring_buffer;

/** 从 g_ring_buffer 同步 ring_used / ring_capacity / ring_full 到 g_job_status */
void job_status_update_ring(void);

/** 递增 ui_generation，通知 UI 刷新 */
void job_status_ui_bump(void);

/** 读取当前 ui_generation（无锁） */
uint32_t job_status_ui_generation(void);

/** 将 X/Y/Z 设为 MCU_COORD_ZERO_OFFSET（main 上电、Reset 用） */
void job_status_reset_mcu_position(void);

/**
 * 应用下位机 19B 坐标：各轴为 0 时不覆盖（空闲时常报 0，非有效位置）
 * @note 调用方必须已持有 JOB_LOCK
 */
void job_status_apply_mcu_position_locked(uint16_t x, uint16_t y, uint16_t z);

/** 将 cmd/data rate 格式化为百分比字符串（小值保留两位小数） */
void format_mcu_queue_rate_label(char *buf, size_t buf_size, uint16_t rate);

/**
 * 根据 cmd_cache_rate 更新 mcu_cmd_tx_hold（滞回：90% 暂停 / 85% 恢复）
 * @note 调用方必须已持有 JOB_LOCK
 */
void job_status_sync_mcu_cmd_backpressure_locked(void);

/** 百分比四舍五入，den<=0 返回 0，num>=den 返回 100 */
int percent_rounded(int num, int den);

/** 百分比×10 四舍五入，用于 Home 进度条一位小数（1000=100.0%） */
int percent_tenths(int num, int den);

/** G-code 逻辑原点对应下位机 raw 坐标（Reset 时写入 current_x/y/z） */
#define MCU_COORD_ZERO_OFFSET 32768

/** 访问 g_job_status 时加锁（RAII，作用域结束自动释放） */
#define JOB_LOCK()    std::lock_guard<std::mutex> _job_lock(g_job_status.mtx)

#ifdef __cplusplus
}
#endif

#endif
