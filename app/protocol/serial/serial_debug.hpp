#ifndef SERIAL_DEBUG_HPP
#define SERIAL_DEBUG_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file serial_debug.hpp
 * @brief 调试页（手动控制台）UART4 串口命令编解码。
 *
 * 仅做编解码、无 I/O；对应 doc/MCU串口通信协议文档_new.md 中调试页会用到的命令。
 * 帧格式复用 serial_frame：5A A5 AA Port Cmd Seq Len Payload(单条 TLV) Sum。
 * 振镜（以太网 TCP）当前无串口命令，故本文件不提供其 build_*。
 *
 * 所有 build_* 成功返回整帧字节数，缓冲不足/参数非法返回 -1。
 * 单帧最大 = 8 + (2 + 2) = 12 字节，建议 out 缓冲 SERIAL_DEBUG_MAX_FRAME。
 */
#define SERIAL_DEBUG_MAX_FRAME 16

/* ---- Port（任务路由） ---- */
#define SDBG_PORT_GENERAL   0xA2  /* PORT_GENERAL_TASK：唤醒/电机使能/Z·S·D 轴 */
#define SDBG_PORT_SENSORS   0xA5  /* PORT_SENSORS_TASK：门/照明/风扇/三通/激光使能/基板加热 */
#define SDBG_PORT_POWDER    0xA9  /* PORT_POWDER_TASK：粉桶 */
#define SDBG_PORT_GAS       0xA1  /* PORT_GAS_TASK：洗气 */
#define SDBG_PORT_PRINTING  0xA4  /* PORT_PRINTING_TASK：打印层参数等 */
#define SDBG_PORT_LASER     0xAA  /* PORT_LASER_TASK：激光器手动功率 */
#define SDBG_PORT_ALARM     0xA3  /* PORT_ALARM_TASK：报警复位 */

/* ====================================================================== */
/*  Tab1 移动控制：Z / S / D 三轴（Port = SDBG_PORT_GENERAL）              */
/* ====================================================================== */

/**
 * 轴长按点动（速度模式）。axis: 0=Z 1=S 2=D。
 * speed 为有符号 int16（mm/s，D 为 °/s），符号即方向。
 */
int serial_debug_build_axis_jog(uint8_t axis, int16_t speed,
                                uint8_t* out, size_t out_size);

/** 轴点动停止。axis: 0=Z 1=S 2=D。 */
int serial_debug_build_axis_jog_stop(uint8_t axis, uint8_t* out, size_t out_size);

/**
 * 轴点击增量移动（相对位置）。axis: 0=Z 1=S 2=D。
 * delta 为有符号 int16（mm，D 为 °），符号即方向。
 * Z 轴 MCU 只受理 Tlv-Len=4 的相对位移，Z 改用 serial_debug_build_z_inc_um。
 */
int serial_debug_build_axis_inc(uint8_t axis, int16_t delta,
                                uint8_t* out, size_t out_size);

/**
 * Z 轴相对步进。TLV 0x02，Value = INT32 LE，单位 μm（协议规定 Tlv-Len=4）。
 * 例：+500 = 上升 0.5mm，-7263 = 下降 7.263mm。
 * 校准/测高与所有 Z 点击移动（调试页 Move/Test、监控页 Manual）共用。
 */
int serial_debug_build_z_inc_um(int32_t delta_um, uint8_t* out, size_t out_size);

/**
 * 轴回 Home（归零）。仅 Z(0)/S(1) 有效（Z→0x11，S→0x10）；D 无回 Home，返回 -1。
 */
int serial_debug_build_axis_home(uint8_t axis, uint8_t* out, size_t out_size);

/** Z 轴设置工作零点（仅更新工作坐标补偿，不运动）。 */
int serial_debug_build_z_set_zero(uint8_t* out, size_t out_size);

/* ====================================================================== */
/*  设备快捷行：设备唤醒 / 门 / 电机使能 / 照明                            */
/* ====================================================================== */

/** 设备唤醒(active=true) / 休眠(active=false)。Port=GENERAL，Cmd=0xAA。 */
int serial_debug_build_machine_wake(bool active, uint8_t* out, size_t out_size);

/** 开门（关门不再使用，故仅提供开门）。Port=SENSORS，TLV=0x08。 */
int serial_debug_build_door_open(uint8_t* out, size_t out_size);

/** 电机使能(true)/失能(false)。Port=GENERAL，Cmd=0xB0。 */
int serial_debug_build_motor_enable(bool enable, uint8_t* out, size_t out_size);

/** 照明开/关。Port=SENSORS，TLV=0x06/0x07。 */
int serial_debug_build_light(bool on, uint8_t* out, size_t out_size);

/* ====================================================================== */
/*  Tab2 其他控制：粉桶 / 筛粉 / 回粉 / 风扇 / 激光·洗气·基板加热 / 清粉   */
/* ====================================================================== */

/** 粉桶开/关。bucket_id: 1 或 2。Port=POWDER，Cmd=0x8A，TLV=0x01/0x02。 */
int serial_debug_build_powder_bucket(uint8_t bucket_id, bool on,
                                     uint8_t* out, size_t out_size);

/**
 * 筛粉电机（原三通推杆已改为筛粉电机）。action 与 DebugMotorAction 对齐：
 * 0=停止(TLV 0x15) / 1=筛粉运行·正转(0x13) / 2=反转倒渣(0x14)。
 * Port=SENSORS，Cmd=0x9A，无 Value。
 */
int serial_debug_build_sieve_motor(uint8_t action, uint8_t* out, size_t out_size);

/**
 * 内门（原回粉电机块改为内门）。action 与 DebugMotorAction 对齐：
 * 0=停止(TLV 0x0E) / 1=开(0x0C) / 2=关(0x0D)。Port=SENSORS，Cmd=0x9A，无 Value。
 */
int serial_debug_build_inner_door(uint8_t action, uint8_t* out, size_t out_size);

/**
 * 回粉电机手动送瓶（工厂调试，绕过 A/B 满瓶自动选瓶）。action 与 DebugMotorAction 对齐：
 * 0=停止并恢复自动(TLV 0x18) / 1=送往 B 瓶(0x17) / 2=送往 A 瓶(0x16)。
 * Port=SENSORS，Cmd=0x9A，无 Value；TO_A/TO_B 可互相切换，可与筛粉手动同时运行。
 * MCU 回 FrameType=0xBA 的 ACK 仅表示已入队，无独立执行结果上报。
 */
int serial_debug_build_return_motor(uint8_t action, uint8_t* out, size_t out_size);

/** 风扇开/关。Port=SENSORS，TLV=0x02/0x03。 */
int serial_debug_build_fan_switch(bool on, uint8_t* out, size_t out_size);

/** 风扇 PWM 占空比。duty 合法范围 0~100（调用方应自行限幅）。Port=SENSORS，TLV=0x01。 */
int serial_debug_build_fan_duty(uint8_t duty, uint8_t* out, size_t out_size);

/** 其他开关：target 0=激光使能 1=洗气 2=基板加热。 */
int serial_debug_build_switch(uint8_t target, bool on,
                              uint8_t* out, size_t out_size);

/**
 * 清粉请求开启。Port=GENERAL(0xA2)，Cmd=CMD_REQUEST_DEPOWDERING(0x42)，TLV=0x01（TLV_DUST_CLEAN_REQ）。
 * 获批时受理：MCU 临时接管 Z 轴，开风机，按 5mm 步进上升到记录高度再降回，循环 3 轮后停风机。
 * 结束由 TLV_DUST_CLEAN_FINISHED(0x03) 回执，见 serial_parse_clean_test_result()。
 */
int serial_debug_build_clean_test_start(uint8_t* out, size_t out_size);

/**
 * 清粉中止。Port=GENERAL(0xA2)，Cmd=0x42，TLV=0x02（TLV_DUST_CLEAN_STOP）。
 * 任意整机状态均受理；终止后回 TLV_DUST_CLEAN_FINISHED result=2。清粉未在运行时无副作用、不回执。
 * 不撤销已下发的 Z 轴运动，正在执行的单步可能仍会走完。
 */
int serial_debug_build_clean_test_close(uint8_t* out, size_t out_size);

/**
 * 打印层参数：layer_um(uint16 LE) + balance_x1000(uint16 LE)。
 * Port=PRINTING，Cmd=0xB5，TLV=0x01，Len=4。balance_x1000 = 平衡系数×1000。
 */
int serial_debug_build_print_layer_param(uint16_t layer_um, uint16_t balance_x1000,
                                         uint8_t* out, size_t out_size);

/* ====================================================================== */
/*  Laser（激光器）标签页                                                  */
/* ====================================================================== */

/** 激光器手动功率设置。percent 0~100。Port=LASER，Cmd=0x87，TLV=0x01，Len=1。 */
int serial_debug_build_laser_power_set(uint8_t percent, uint8_t* out, size_t out_size);

/** 取消手动功率、恢复外控。Port=LASER，Cmd=0x87，TLV=0x02，无 Value。 */
int serial_debug_build_laser_power_reset(uint8_t* out, size_t out_size);

/** 激光器报警复位（固定 alarm_code=1301）。Port=ALARM，Cmd=0x82，TLV=0x01，Len=2。 */
int serial_debug_build_laser_alarm_reset(uint8_t* out, size_t out_size);

/** 激光器状态上报（MCU→SOC，Cmd 0x9B + TLV 0x03，Len 8）。 */
typedef struct {
    uint16_t status;        /**< bit0 红光/bit1 激光输出/bit2 Ready/bit3 外部输出有效 */
    uint16_t alarm;         /**< laser_alarm 打包报警位 */
    uint16_t power_raw;     /**< 功率原始值 0..255 */
    uint16_t pump_temp_raw; /**< 泵浦温度原始值 */
} serial_laser_report_t;

int serial_frame_is_laser_report(const uint8_t* data, size_t len);
int serial_parse_laser_report(const uint8_t* data, size_t len,
                              serial_laser_report_t* out);

/* ====================================================================== */
/*  状态回读：运动轴位置上报（SensorsTask，MCU→SOC）                        */
/*  帧：Port 0xA5 + Cmd 0x9B + TLV 0x05，Len 16；Value 顺序 Z/Zwork/S/D。 */
/*  注：顶部 7 路传感器上报（TLV 0x02）由 serial_env.* 解析，此处不重复。   */
/* ====================================================================== */

typedef struct {
    float z_pos_mm;        /**< Z 轴实际位置（mm，原始 0.1um） */
    float z_work_zero_mm;  /**< Z 轴工作坐标（mm，原始 0.1um） */
    float s_pos_mm;        /**< S 轴实际位置（mm，原始 0.1mm） */
    float d_pos_deg;       /**< D 轴实际位置（°，原始 0.1deg） */
} serial_motion_pos_t;

/** 快速判断是否为运动位置上报帧（5A A5 BB A5 9B ... TLV 0x05）。 */
int serial_frame_is_motion_pos_report(const uint8_t* data, size_t len);

/** 解析运动位置上报；成功返回 0，失败返回负值（参数/SOF/PortCmd/TLV/长度/校验）。 */
int serial_parse_motion_pos_report(const uint8_t* data, size_t len,
                                   serial_motion_pos_t* out);

#ifdef __cplusplus
}
#endif

#endif
