#ifndef SERIAL_ALARM_HPP
#define SERIAL_ALARM_HPP

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file serial_alarm.hpp
 * @brief 报警任务命令（PORT_ALARM_TASK 0xA3）：单条报警上报 + 报警复位请求
 *
 * 单条上报（MCU→SOC）：5A A5 BB A3 20 <seq> 0B 01 09 <9B Value> <cks>
 *   TLV_ALARM_SINGLE_REPORT(0x01) len=9：
 *   Value = alarm_code(u16 LE) severity(u8) state_mask(u8) clear_mode(u8) detail(u32 LE)。
 *   CONDITION 与 EVENT 共用本帧，由 state_mask bit2 区分。
 * 快照上报（MCU→SOC）：5A A5 BB A3 81 <seq> 0D 04 0B <11B Value> <cks>
 *   TLV_ALARM_EVT_SNAPSHOT(0x04) len=11：
 *   Value = word_count(u8) word_index(u8) highest_severity(u8)
 *           active_mask(u32 LE) latched_mask(u32 LE)。
 *   分片发送，收齐 word_count 帧才是一批完整快照。
 *
 * 注意：`doc/主控板串口通信协议 - UART通信命令详表.csv` 里报警那几段是**旧版本**
 * （把 state_mask bit2 写成 acked、clear_mode 写成 life state、快照 len 写成 15），
 * 与 `信息报警命令详表.csv` 和开发指导 md 冲突。以后两者为准，勿照那份改。
 * 复位请求（SOC→MCU）：5A A5 AA A3 82 <seq> 04 01 02 <alarm_code LE> <cks>
 *   TLV_ALARM_EVT_RESET(0x01) len=2。RESET 仅适用于 CONDITION；异常条件仍存在
 *   时 MCU 可能拒绝，最终状态以后续单条上报为准。
 *
 * 字典与解析约定见 doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md、
 * doc/主控板串口通信协议 - 信息报警命令详表.csv。
 */

#define SERIAL_ALARM_PORT               0xA3  /**< PORT_ALARM_TASK */
#define SERIAL_ALARM_CMD_SINGLE_REPORT  0x20  /**< CMD_ALARM_SINGLE_REPORT（MCU→SOC） */
#define SERIAL_ALARM_CMD_EVT_REPORT     0x81  /**< CMD_ALARM_EVT_REPORT 快照（MCU→SOC） */
#define SERIAL_ALARM_CMD_EVT_RESPONSE   0x82  /**< CMD_ALARM_EVT_RESPONSE（SOC→MCU） */
#define SERIAL_ALARM_TLV_SINGLE_REPORT  0x01  /**< TLV_ALARM_SINGLE_REPORT，len=9 */
#define SERIAL_ALARM_TLV_EVT_SNAPSHOT   0x04  /**< TLV_ALARM_EVT_SNAPSHOT，len=11 */
#define SERIAL_ALARM_TLV_EVT_RESET      0x01  /**< TLV_ALARM_EVT_RESET，len=2 */
#define SERIAL_ALARM_SINGLE_TLV_LEN     0x09  /**< 单条上报 Value 长度 */
#define SERIAL_ALARM_SNAPSHOT_TLV_LEN   0x0B  /**< 快照 Value 长度（11） */

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
 * 报警快照的一帧（0xA3/0x81，TLV 0x04 len=11）。
 *
 * 快照按 32 位一组分片：`word_count` 帧才是完整一批，接收方要按 `word_index`
 * 攒齐再用——用半套覆盖当前表会把没收到那几组的报警误判成已解除。
 * 只含 CONDITION，EVENT 不占 bit；位图槽位到报警号的映射见 `报警快照Map.csv`
 * （已由 tools/gen_alarm_catalog.py 生成成 kSnapshotSlots）。
 */
typedef struct {
    uint8_t  word_count;       /**< 本批共几帧；别写死，报警表会增长 */
    uint8_t  word_index;       /**< 本帧是第几组（0 基） */
    uint8_t  highest_severity; /**< 当前最高等级，可作提示，非必需 */
    uint32_t active_mask;      /**< 本组 32 条的 active 位（小端解出） */
    uint32_t latched_mask;     /**< 本组 32 条的 latched 位 */
} serial_alarm_snapshot_t;

/**
 * @brief 解析单条报警上报帧（TLV_ALARM_SINGLE_REPORT）
 * @return 0 成功；-1 参数/长度不足；-2 SOF 或 Port/Cmd 不匹配；-3 TLV 不匹配；-4 校验失败
 */
int serial_parse_alarm_single_report(const uint8_t* data, size_t len,
                                     serial_alarm_report_t* out);

/**
 * @brief 解析报警快照的一帧（TLV_ALARM_EVT_SNAPSHOT）
 * @return 0 成功；-1 参数/长度不足；-2 SOF 或 Port/Cmd 不匹配；-3 TLV 不匹配；-4 校验失败
 */
int serial_parse_alarm_snapshot(const uint8_t* data, size_t len,
                                serial_alarm_snapshot_t* out);

/**
 * @brief 组装报警复位请求帧（TLV_ALARM_EVT_RESET，alarm_code 小端）
 * @return >0 帧长；<=0 组帧失败
 */
int serial_build_alarm_reset(uint16_t alarm_code, uint8_t* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
