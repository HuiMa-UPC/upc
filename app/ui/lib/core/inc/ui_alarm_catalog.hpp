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
 * 必须立刻改用报文值覆盖，不得让兜底值长期生效。字典里连这个 code 都查不到时，
 * `ui_alarm_state::snapshot_upsert()` 另有一套放宽的兜底（warning + RESET），
 * 理由见那里。
 *
 * `reset_owner_task` 是"以报文为准"的**唯一例外**——报文里根本没有这个字段，
 * 只有 `报警map.csv` 的 reset_policy 列有。整条 code 查不到字典时按 OWNER_TASK
 * 放行（见 `ui_alarm_reset_allowed()`）：那是字典没跟上固件的信息缺失，拦下来
 * 新报警就永远复不了位。
 *
 * 快照槽位映射**不在本文件**：报警号 ↔ (slot_id,bit) 是固定公式，见
 * `middleware::alarm_snapshot_code_of()` / `alarm_snapshot_covers()`
 * （`common/upc_contract.hpp`）。生成脚本只负责校验 CSV 与公式一致。
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
     *  （命名以 doc/主控板串口通信协议 - 信息报警命令.csv 为准）。
     *  **仅快照场景兜底**，见文件头。 */
    uint8_t     severity_fallback;
    /** 0 AUTO / 1 RESET / 2 POWER_CYCLE / 3 NONE。**仅快照场景兜底**。 */
    uint8_t     clear_mode_fallback;
    uint8_t     is_event;  /**< 1=EVENT（一次性，不占快照 bit）；0=CONDITION */
    /** 1=reset_policy 为 OWNER_TASK：复位请求会被 MCU 转给业务任务做硬件恢复，
     *  故障条件仍在（ACTIVE）时也该允许下发——激光器 1301、各轴故障都属此类，
     *  不点复位它们永远好不了。0=LOCAL：条件仍在时 MCU 直接拒绝，按钮该置灰。
     *  **不在报文里**，只能查字典；整条 code 查不到时按放行处理（见文件头）。 */
    uint8_t     reset_owner_task;
} ui_alarm_catalog_entry_t;

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

#ifdef __cplusplus
}
#endif

#endif
