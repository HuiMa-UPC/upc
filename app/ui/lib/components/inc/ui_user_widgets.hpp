#ifndef UI_USER_WIDGETS_H
#define UI_USER_WIDGETS_H

#include "ui_modal_dialog.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/** 分段控件「无选中」哨兵：作为初值可建出全灰未选状态，读取时同样返回它 */
#define UI_USER_SEG_NONE  0xFFu

/** 用户弹窗默认配置（标题居中、无装饰下划线） */
ui_modal_dialog_cfg_t ui_modal_dialog_cfg_user(const char *title,
                                               int32_t card_width,
                                               bool with_footer);

/** 弹窗正文说明 label */
lv_obj_t *ui_user_make_body_label(lv_obj_t *parent, const char *text);

/** 格式化时长：>=60 分钟输出 "Xh Ymin"，否则 "Ymin" */
void ui_user_format_duration_min(char *buf, size_t buf_size, uint32_t duration_min);

/** 格式化文件副信息："Xh Ymin · Zg" 或 "Ymin · Zg" */
void ui_user_format_file_meta(char *buf, size_t buf_size,
                              uint32_t duration_min, float material_g);

/** 主色渐变预览块 */
lv_obj_t *ui_user_make_accent_preview(lv_obj_t *parent, int32_t w, int32_t h);

/** 用户风格进度条（panel_alt 底 + accent 指示器） */
lv_obj_t *ui_user_make_progress_bar(lv_obj_t *parent, int32_t w, int32_t h);

/** spinner 透明容器 */
lv_obj_t *ui_user_make_spinner_container(lv_obj_t *parent, int32_t inner_size);

/**
 * footer 区定宽副按钮（USER_DLG_FOOTER_BTN_W × _H）：4% 白底 + USER_BTN_RADIUS 圆角。
 * 与 ui_user_make_confirm_footer_btn(filled=false) 同一观感，区别只是定宽而非等分。
 */
lv_obj_t *ui_user_make_footer_ghost_btn(lv_obj_t *footer, const char *text,
                                        lv_event_cb_t cb);

/** 半透明圆角面板（设置列表 / 骨架页卡片） */
lv_obj_t *ui_user_make_frosted_card(lv_obj_t *parent, bool flex_grow);

/** 装饰横线（1px） */
lv_obj_t *ui_user_make_divider_line(lv_obj_t *parent, lv_coord_t width);

/** 装饰竖线（1px 宽 × height 高）：卡片指标之间的分隔（稿 4447:6189） */
lv_obj_t *ui_user_make_divider_vline(lv_obj_t *parent, lv_coord_t height);

/**
 * A8 图标盒：外层固定 target×target 占位（避免大尺寸源图在 flex 里撑开父容器），
 * 内层图片 recolor 成 color 并等比缩放到 target。
 * 源图边长从图头取，24/32/40/72/96 各档通吃。
 */
lv_obj_t *ui_user_make_icon_img(lv_obj_t *parent, const lv_image_dsc_t *src,
                                int32_t target, lv_color_t color);

/**
 * 主按钮胶囊：蓝色对角渐变 + 白色亮边 + 柔和蓝投影 + 可选图标（稿「Button / On Light」）。
 * 首页 "Start Print" 与打印历史空态的 "Start Print" 共用，保证两处是同一枚按钮。
 * @param icon 可为 NULL（纯文字按钮）
 */
lv_obj_t *ui_user_make_primary_pill_btn(lv_obj_t *parent, const lv_image_dsc_t *icon,
                                        const char *text, lv_event_cb_t cb);

/**
 * 二次确认弹窗页脚按钮（稿 2567:5555 的「弹窗副按钮 / 弹窗重点按钮」）：
 *   filled=true  → fill_color 填充的重点按钮；
 *   filled=false → 4% 白填充、无描边的副按钮。
 * 两种都是 USER_BTN_RADIUS 圆角矩形（不是全圆胶囊），并用 flex_grow 等分整行宽度。
 * 共用方：选文件页删除确认、终止打印确认、打印历史批量删除确认。
 */
lv_obj_t *ui_user_make_confirm_footer_btn(lv_obj_t *footer, const char *text,
                                          bool filled, lv_color_t fill_color,
                                          lv_event_cb_t cb, void *user_data);

/** 首页数值块：横线 + 标签 + 大数字；out_value 可接刷新指针 */
lv_obj_t *ui_user_make_metric_block(lv_obj_t *parent, const char *caption,
                                    lv_text_align_t text_align,
                                    lv_obj_t **out_value);

/** 状态圆点（就绪绿 / 错误红等由 ready 决定） */
lv_obj_t *ui_user_make_status_dot(lv_obj_t *parent, bool ready);

/** 耗材/HEPA 一行：圆点 + 名称 + Ready/Not Ready */
lv_obj_t *ui_user_make_supply_indicator(lv_obj_t *parent, const char *name,
                                        bool ready);

/**
 * 供应度卡片（设计稿 1.1.5 顶部「耗材可用/滤芯可用 + 进度条 + 百分比」）。
 * 名称行 + 状态(Adequate/Low) + 进度条 + 百分比。out_* 接刷新句柄（可为 NULL）。
 */
lv_obj_t *ui_user_make_supply_bar(lv_obj_t *parent, const char *name,
                                  bool ok, int percent,
                                  lv_obj_t **out_bar,
                                  lv_obj_t **out_pct_lbl,
                                  lv_obj_t **out_status_lbl);

/** 设置页列表行：左标题 + 右可选按钮 */
lv_obj_t *ui_user_make_setting_row(lv_obj_t *parent, const char *label,
                                   const char *btn_text, lv_event_cb_t btn_cb);

/**
 * 分段选择控件（如 Wi-Fi 开/关、移动步长 0.01/0.1/1/10）。
 * 暗色胶囊容器内水平排布 count 段，选中段以浅白填充高亮。
 * 点击某段会切换选中并向 **容器对象** 发送 LV_EVENT_VALUE_CHANGED。
 * 调用方：
 *   lv_obj_t *seg = ui_user_make_segmented(parent, labels, n, 0);
 *   lv_obj_add_event_cb(seg, my_cb, LV_EVENT_VALUE_CHANGED, ud);
 *   uint8_t idx = ui_user_segmented_get_selected(seg);
 * @param labels   段文字数组（count 个）
 * @param count    段数量（1..8）
 * @param selected 初始选中段索引；传 UI_USER_SEG_NONE 则不选中任何段
 * @return 容器对象（即事件源）
 */
lv_obj_t *ui_user_make_segmented(lv_obj_t *parent,
                                 const char *const labels[], uint8_t count,
                                 uint8_t selected);

/** 设置分段控件当前选中段（不触发回调）；UI_USER_SEG_NONE = 全部取消选中 */
void ui_user_segmented_set_selected(lv_obj_t *seg, uint8_t index);

/** 读取分段控件当前选中段索引；无选中时返回 UI_USER_SEG_NONE */
uint8_t ui_user_segmented_get_selected(lv_obj_t *seg);

#ifdef __cplusplus
}
#endif

#endif
