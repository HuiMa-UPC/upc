#ifndef SERIAL_ALARM_HPP
#define SERIAL_ALARM_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file serial_alarm.hpp
 * @brief 报警任务命令（PORT_ALARM_TASK 0xA3）：单条上报 + 快照请求/响应 + 复位
 *
 * 单条上报（MCU→SOC）：5A A5 BB A3 20 <seq> 0B 01 09 <9B Value> <cks>
 *   TLV_ALARM_SINGLE_REPORT(0x01) len=9：
 *   Value = alarm_code(u16 LE) severity(u8) state_mask(u8) clear_mode(u8) detail(u32 LE)。
 *   CONDITION 与 EVENT 共用本帧，由 state_mask bit2 区分。
 * 快照请求（SOC→MCU）：5A A5 AA A3 81 <seq> 02 05 00 <cks>
 *   TLV_ALARM_EVT_REQSPT(0x05) len=0。**MCU 不会主动推快照**，不发这条就收不到。
 * 快照响应（MCU→SOC）：5A A5 BB A3 81 <seq> 0D 04 0B <11B Value> <cks>
 *   TLV_ALARM_EVT_SNAPSHOT(0x04) len=11：
 *   Value = nonzero_slot_count(u8) slot_id(u8) highest_severity(u8)
 *           active_mask(u32 LE) latched_mask(u32 LE)。
 *   响应**稀疏**：只回 (active|latched)!=0 的 slot；count=0 表示当前无报警。
 * 复位请求（SOC→MCU）：5A A5 AA A3 82 <seq> 04 01 02 <alarm_code LE> <cks>
 *   TLV_ALARM_EVT_RESET(0x01) len=2。RESET 仅适用于 CONDITION。
 * 复位失败回执（MCU→SOC）：5A A5 BB A3 82 <seq> 05 04 03 <alarm_code LE> <result> <cks>
 *   TLV_ALARM_EVT_RESET_RESULT(0x04) len=3。只在失败时发；Seq 固定 0，按
 *   alarm_code 关联在途请求。成功仍以后续 NORMAL 单报为准。
 *
 * 注意 TLV 0x04 被快照与复位失败**共用**，必须靠 Cmd（0x81 / 0x82）区分。
 *
 * 权威顺序（几份文档描述同一套协议，冲突时按此取舍）：
 *   1. doc/主控板串口通信协议 - *.csv（四份，最新）
 *   2. doc/SOC_TLV_ALARM_EVT_SNAPSHOT报警快照开发指导.md
 *   3. doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md（未随 CSV 更新，
 *      其 §5 复位按钮伪代码与 §7 fault/trip 叫法均已过时，仅作背景参考）
 *   4. doc/主控板串口通信协议 - UART通信命令详表.csv 里的报警段是**更旧的版本**
 *      （state_mask bit2 写成 acked、快照 len 写成 15），勿照那份改。
 */

#define SERIAL_ALARM_PORT               0xA3  /**< PORT_ALARM_TASK */
#define SERIAL_ALARM_CMD_SINGLE_REPORT  0x20  /**< CMD_ALARM_SINGLE_REPORT（MCU→SOC） */
#define SERIAL_ALARM_CMD_EVT_REPORT     0x81  /**< CMD_ALARM_EVT_REPORT 快照请求/响应 */
#define SERIAL_ALARM_CMD_EVT_RESPONSE   0x82  /**< CMD_ALARM_EVT_RESPONSE 复位请求/回执 */
#define SERIAL_ALARM_TLV_SINGLE_REPORT  0x01  /**< TLV_ALARM_SINGLE_REPORT，len=9 */
#define SERIAL_ALARM_TLV_EVT_SNAPSHOT   0x04  /**< TLV_ALARM_EVT_SNAPSHOT（Cmd 0x81），len=11 */
#define SERIAL_ALARM_TLV_EVT_REQSPT     0x05  /**< TLV_ALARM_EVT_REQSPT（Cmd 0x81），len=0 */
#define SERIAL_ALARM_TLV_EVT_RESET      0x01  /**< TLV_ALARM_EVT_RESET（Cmd 0x82），len=2 */
#define SERIAL_ALARM_TLV_EVT_RESET_RES  0x04  /**< TLV_ALARM_EVT_RESET_RESULT（Cmd 0x82），len=3 */
#define SERIAL_ALARM_SINGLE_TLV_LEN     0x09  /**< 单条上报 Value 长度 */
#define SERIAL_ALARM_SNAPSHOT_TLV_LEN   0x0B  /**< 快照 Value 长度（11） */
#define SERIAL_ALARM_RESET_RES_TLV_LEN  0x03  /**< 复位失败回执 Value 长度（3） */

/** 快照 slot 总数：4 个报警组 × 每组 3 个 mask 字（slot_id 合法范围 0..11）。 */
#define SERIAL_ALARM_SNAPSHOT_SLOTS     12

/** result 取值：MCU 只在复位失败时回执。 */
#define SERIAL_ALARM_RESET_RESULT_FAILED 0x01

/* state_mask 位定义（bit3..7 保留，解析时忽略） */
#define SERIAL_ALARM_STATE_ACTIVE   0x01  /**< bit0：故障条件当前存在 */
#define SERIAL_ALARM_STATE_LATCHED  0x02  /**< bit1：已锁存，等待清除 */
#define SERIAL_ALARM_STATE_EVENT    0x04  /**< bit2：一次性 EVENT，无生命周期 */

/* clear_mode 枚举 */
#define SERIAL_ALARM_CLEAR_AUTO         0
#define SERIAL_ALARM_CLEAR_RESET        1
#define SERIAL_ALARM_CLEAR_POWER_CYCLE  2
#define SERIAL_ALARM_CLEAR_NONE         3

typedef struct {
    uint16_t alarm_code;  /**< 稳定报警号（小端解出） */
    uint8_t  severity;    /**< 1=information 2=warning 3=error 4=critical */
    uint8_t  state_mask;  /**< SERIAL_ALARM_STATE_* 位组合 */
    uint8_t  clear_mode;  /**< SERIAL_ALARM_CLEAR_* */
    uint32_t detail;      /**< 详情号/原因值（小端解出），0=无详情 */
} serial_alarm_report_t;

/**
 * 报警快照的一个 slot（0xA3/0x81，TLV 0x04 len=11）。
 *
 * 稀疏响应：MCU 只发 (active|latched)!=0 的 slot，接收方要攒齐
 * `nonzero_slot_count` 个**不同** slot 才能原子提交——用半套覆盖当前表会把
 * 没收到那些 slot 的报警误判成已解除。没回的 slot 一律按全零处理。
 * 只含 CONDITION，EVENT 不占 bit；槽位→报警号用固定公式（见头部注释与
 * middleware::alarm_snapshot_code_of），不查表。
 */
typedef struct {
    uint8_t  nonzero_slot_count; /**< 本轮非零 slot 总数（0..12）；0=当前无报警 */
    uint8_t  slot_id;            /**< 本帧的 slot（0..11），非连续、不保证顺序 */
    uint8_t  highest_severity;   /**< 快照时刻最高等级；0=无有效 CONDITION */
    uint32_t active_mask;        /**< 本 slot 32 条的 active 位（小端解出） */
    uint32_t latched_mask;       /**< 本 slot 32 条的 latched 位 */
} serial_alarm_snapshot_t;

/**
 * 报警复位失败回执（0xA3/0x82，TLV 0x04 len=3）。
 *
 * MCU 只在失败时发；成功不回执，以后续 NORMAL 单报为准。Seq 固定 0，
 * 所以只能按 alarm_code 关联在途的复位请求。
 */
typedef struct {
    uint16_t alarm_code;  /**< 关联正在复位的报警（小端解出） */
    uint8_t  result;      /**< SERIAL_ALARM_RESET_RESULT_FAILED */
} serial_alarm_reset_result_t;

/**
 * @brief 解析单条报警上报帧（TLV_ALARM_SINGLE_REPORT）
 * @return 0 成功；-1 参数/长度不足；-2 SOF 或 Port/Cmd 不匹配；-3 TLV 不匹配；-4 校验失败
 */
int serial_parse_alarm_single_report(const uint8_t* data, size_t len,
                                     serial_alarm_report_t* out);

/**
 * @brief 解析报警快照的一个 slot（Cmd 0x81 + TLV_ALARM_EVT_SNAPSHOT）
 *
 * 只做帧级校验（Port/Cmd/TLV/校验和）与字段拆解，并拒掉 slot_id/count 越界；
 * 攒帧、轮次判定与原子提交由调用方（UI 侧 ui_alarm_state）负责。
 *
 * @return 0 成功；-1 参数/长度不足；-2 SOF 或 Port/Cmd 不匹配；-3 TLV 不匹配；
 *         -4 校验失败；-5 slot_id 或 nonzero_slot_count 越界
 */
int serial_parse_alarm_snapshot(const uint8_t* data, size_t len,
                                serial_alarm_snapshot_t* out);

/**
 * @brief 解析报警复位失败回执（Cmd 0x82 + TLV_ALARM_EVT_RESET_RESULT）
 *
 * 与快照共用 TLV 号 0x04，靠 Cmd 区分，所以必须先判 Cmd==0x82。
 *
 * @return 0 成功；-1 参数/长度不足；-2 SOF 或 Port/Cmd 不匹配；-3 TLV 不匹配；-4 校验失败
 */
int serial_parse_alarm_reset_result(const uint8_t* data, size_t len,
                                    serial_alarm_reset_result_t* out);

/**
 * @brief 组装报警复位请求帧（TLV_ALARM_EVT_RESET，alarm_code 小端）
 * @return >0 帧长；<=0 组帧失败
 */
int serial_build_alarm_reset(uint16_t alarm_code, uint8_t* out, size_t out_size);

/**
 * @brief 组装报警快照请求帧（TLV_ALARM_EVT_REQSPT，无 Value）
 *
 * 产物固定为 `5A A5 AA A3 81 01 02 05 00 D5`（Seq 由 serial_build_aa_frame
 * 写死 0x01；当前不做 Seq 匹配，轮次由 SOC 发请求的时刻界定）。
 *
 * @return >0 帧长；<=0 组帧失败
 */
int serial_build_alarm_snapshot_request(uint8_t* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
