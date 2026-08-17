#ifndef UI_ALARM_CATALOG_HPP
#define UI_ALARM_CATALOG_HPP

#include <stdbool.h>
#include <stdint.h>

/**
 * @file ui_alarm_catalog.hpp
 * @brief MCU 报警字典：报警号/详情号 → 英文显示文案。
 *
 * 表体在 `src/ui_alarm_catalog_gen.cpp`，由 `tools/gen_alarm_catalog.py` 从
 * `doc/主控板串口通信协议 - 报警&事件表.csv` + `报警Details.csv` 生成，英文
 * 覆盖在 `doc/alarm_catalog_en.csv`（未翻译条目用 macro 自动转换兜底）。
 * **生成物禁止手改**；MCU 组更新 CSV 后重跑脚本即可。
 *
 * 字典主要负责文字。severity / clear_mode / 生命周期**一律以报文字段为准**——
 * MCU 固件可能比字典新，查不到的 code 必须降级显示原始编号而不是丢弃
 * （见 doc/SOC_TLV_ALARM_SINGLE_REPORT报警显示开发指导.md §1/§3）。
 *
 * 表里那两个 `*_fallback` 字段是**快照专用兜底**：快照（0xA3/0x81）只有 active /
 * latched 两个位图，给不出等级和清除方式，而快照又恰恰用在"本地表里没有这条"的
 * 场景（重启/重连后补齐）。这时只能查字典凑出一张卡片。收到该报警的单条上报后
 * 必须立刻改用报文值覆盖，不得让兜底值长期生效。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t    code;   /**< 稳定报警号 */
    const char *macro;  /**< MCU 侧枚举名（联调对照用） */
    const char *title;  /**< 显示名（英文；翻译表覆盖或 macro 自动转换） */
    const char *desc;   /**< 处理建议，可为空串（仅翻译表提供） */
    /** 报文 severity 语义：1 information / 2 warning / 3 error / 4 critical
     *  （命名以 doc/主控板串口通信协议 - 信息报警命令详表.csv 为准）。
     *  **仅快照场景兜底**，见文件头。 */
    uint8_t     severity_fallback;
    /** 0 AUTO / 1 RESET / 2 POWER_CYCLE / 3 NONE。**仅快照场景兜底**。 */
    uint8_t     clear_mode_fallback;
    uint8_t     is_event;  /**< 1=EVENT（一次性，不占快照 bit）；0=CONDITION */
} ui_alarm_catalog_entry_t;

/**
 * 快照位图槽位 → 报警号。快照帧按 word（32 bit）分片，(word_index, bit_index)
 * 唯一定位一条 CONDITION；EVENT 不分配 bit（见信息报警命令详表 §报警快照上报）。
 */
typedef struct {
    uint8_t  word_index;
    uint8_t  bit_index;
    uint16_t code;
} ui_alarm_snapshot_entry_t;

typedef struct {
    uint32_t    code;   /**< 详情号（报文 detail 字段） */
    const char *text;   /**< 原因文字（英文） */
} ui_alarm_detail_entry_t;

/** 生成日期（"YYYY-MM-DD"），启动日志与排查用。 */
extern const char *const ui_alarm_catalog_version;

/** 按报警号查字典；未命中返回 NULL（调用方降级显示 "Unknown alarm #code"）。 */
const ui_alarm_catalog_entry_t *ui_alarm_catalog_find(uint16_t code);

/** 按详情号查字典；未命中返回 NULL（调用方降级显示原始数值）。 */
const ui_alarm_detail_entry_t *ui_alarm_detail_find(uint32_t detail);

/** 字典条数（启动日志用）。 */
int ui_alarm_catalog_count(void);

/**
 * 快照槽位 → 报警号；该槽位未定义返回 0。
 * 用于把 active/latched 位图展开成报警号集合。
 */
uint16_t ui_alarm_snapshot_code(uint8_t word_index, uint8_t bit_index);

/**
 * 报警号 → 快照槽位；该报警不在快照表里（EVENT，或字典比固件旧）返回 false。
 * 对齐时用它判断"这条报警是否受快照管辖"——不受管辖的不能因为快照里没有就清掉。
 */
bool ui_alarm_snapshot_slot_of(uint16_t code, uint8_t *word_index,
                               uint8_t *bit_index);

/** 快照表条数（= CONDITION 总数，启动日志用）。 */
int ui_alarm_snapshot_count(void);

#ifdef __cplusplus
}
#endif

#endif
