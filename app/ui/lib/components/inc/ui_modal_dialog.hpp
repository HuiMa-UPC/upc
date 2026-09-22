#ifndef UI_MODAL_DIALOG_H
#define UI_MODAL_DIALOG_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file ui_modal_dialog.hpp
 * @brief 用户层暗底通用模态弹窗
 *
 * 视觉结构：
 *   ┌────────────────────────────────────────┐
 *   │ 半透明遮罩（占满父容器，吃掉点击）       │
 *   │   ┌──────────────────────────────────┐ │
 *   │   │ 标题（user_font_value/title）    │ │
 *   │   │ ── 主色分割线（可选） ──         │ │
 *   │   │                                  │ │
 *   │   │ body slot：调用方往里加内容        │ │
 *   │   │                                  │ │
 *   │   │ footer slot：调用方加按钮（可选）  │ │
 *   │   └──────────────────────────────────┘ │
 *   └────────────────────────────────────────┘
 *
 * 与 control 的关系：打印流程各步骤弹窗为 ui_modal_dialog_t，
 * 由 control.cpp 打开/关闭。
 */
typedef struct ui_modal_dialog_t {
    lv_obj_t *root;     /**< 含遮罩的最外层；释放它即销毁整个弹窗 */
    lv_obj_t *card;     /**< 弹窗主体（标题/正文/底栏的父） */
    lv_obj_t *title;    /**< 标题 label */
    lv_obj_t *body;     /**< 正文容器，调用方往这里 append 子节点 */
    lv_obj_t *footer;   /**< 底栏容器，调用方往这里 append 按钮（可空） */
    lv_obj_t *close_btn; /**< 右上角关闭按钮（cfg.with_close_btn 为 true 时才有，否则为空）；
                              调用方自行 lv_obj_add_event_cb 处理点击 */
} ui_modal_dialog_t;

/** 一组可选样式参数（NULL 用默认） */
typedef struct {
    const char *title_text;     /**< 标题文字；NULL 则不创建标题 */
    int32_t     card_width;     /**< 0 = USER_DLG_W */
    bool        with_footer;    /**< true 时创建 footer 容器 */
    bool        accent_underline; /**< true 时标题下加主色分割线（用户层已统一 false） */
    bool        with_close_btn; /**< true 时在卡片右上角加一个 "X" 关闭按钮 */
    /**
     * 浅色档（稿 6251:94239）：白卡 + 深色标题 + 更大的圆角/更浅的遮罩。
     *
     * **按调用点开**，不是全局翻。浅色稿是一页一页改过来的，目前只有历史页的批量删除
     * 确认有新稿；其余十四个弹窗里装着白字读数、深色输入框与键盘、进度条，底一翻白
     * 那些内容会当场看不见。哪个弹窗改了稿就把哪个打开，别图省事全开。
     *
     * 只管**外壳**（卡底 / 标题 / 圆角 / 内距 / 遮罩）。body 与 footer 里的东西由调用方
     * 自己上色——footer 那两枚走 ui_user_make_confirm_footer_btn() 的 light 形参。
     */
    bool        light_theme;
} ui_modal_dialog_cfg_t;

/**
 * 在 parent 上创建并显示一个模态弹窗。
 * @param parent  父容器（通常是顶层 overlay 根）
 * @param cfg     配置（不能为 NULL）
 * @return 弹窗句柄；调用方可往 dlg->body / dlg->footer 加子控件
 */
ui_modal_dialog_t *ui_modal_dialog_open(lv_obj_t *parent,
                                        const ui_modal_dialog_cfg_t *cfg);

/** 销毁弹窗（自动 unlink 子节点；调用方不需要再 free body/footer） */
void ui_modal_dialog_close(ui_modal_dialog_t *dlg);

/** 更新标题文字（dialog 复用时刷新用，例如 D2 内部从"检测"切到"净化"） */
void ui_modal_dialog_set_title(ui_modal_dialog_t *dlg, const char *text);

#ifdef __cplusplus
}
#endif

#endif
