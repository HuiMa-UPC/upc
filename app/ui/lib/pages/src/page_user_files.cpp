#include "page_user_files.hpp"

#include "ui_msg_proxy.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "ui_user_widgets.hpp"
#include "ui_alarm_state.hpp"
#include "page_manager.hpp"
#include "home_metric_icons.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

/*
 * 选择文件页（稿 6240:92891 / 93026 / 93195；右栏按钮组按 6357:34689）。
 *
 * 本文件是从 control.cpp 拆出来的——那边原本是 3600+ 行，把这一页的视图代码和打印
 * 状态机、开关门时序、转运盒流程挤在一起。拆分边界与两边的接口见
 * page_user_files.hpp 的文件头注释。
 */
namespace {

/* ---- 页面控件句柄（页面常驻不销毁，建完就一直有效）---- */
lv_obj_t *g_file_panel   = nullptr;   /**< 页根（PageManager 托管） */
bool      g_files_page_active = false; /**< 当前是否可见（refresh/exit 维护） */
lv_obj_t *g_file_grid    = nullptr;   /**< 文件行列表容器（源切换/应答时重建） */
int       g_sel_index    = -1;        /**< 当前选中下标，-1 = 未选 */
FileTab   g_file_tab     = FileTab::Local;

/* 本机/USB 滑动胶囊分段控件：轨道 + 滑动高亮块 + 两个文字 */
lv_obj_t *g_seg_pill     = nullptr;   /**< 滑动高亮块（随选中左右移动） */
lv_obj_t *g_seg_lbl_local = nullptr;
lv_obj_t *g_seg_lbl_usb   = nullptr;

/* ---- 右侧详情面板 ---- */
lv_obj_t *g_detail_hint     = nullptr;  /**< 未选文件时的占位提示 */
lv_obj_t *g_detail_body     = nullptr;  /**< 详情内容容器（选中后显示） */
lv_obj_t *g_detail_name_lbl = nullptr;
lv_obj_t *g_detail_time_lbl = nullptr;       /**< 本机：预估耗时 */
lv_obj_t *g_detail_mat_lbl  = nullptr;       /**< 本机：预估耗材 */
lv_obj_t *g_detail_size_lbl = nullptr;       /**< USB：文件大小 */
lv_obj_t *g_detail_meta_local = nullptr;     /**< 本机详情 meta 行（耗时/耗材） */
lv_obj_t *g_detail_meta_usb   = nullptr;     /**< USB 详情 meta 行（文件大小） */
lv_obj_t *g_detail_secondary_btn = nullptr;  /**< 底部次按钮（本机=删除；USB 隐藏） */
lv_obj_t *g_detail_secondary_lbl = nullptr;  /**< 次按钮文字 */
lv_obj_t *g_detail_start_btn = nullptr;      /**< 底部主按钮（Confirm Print） */
lv_obj_t *g_detail_preview_img = nullptr;    /**< 详情大缩略图 */
lv_obj_t *g_detail_preview_icon = nullptr;   /**< 详情占位图标 */

/* ---- 版式几何（稿 6240:92891，1280 档；其余档位按比例折算）----
 *
 * 两栏在内容区 1152 里正好闭合：`32 + 512 + 64 + 512 + 32`。这里不写死 512——
 * 两栏都是 flex_grow(1)，闭合关系由左右内距与列间距保证，换档位自动成立。 */
int32_t content_w(void)
{
    lv_display_t *d = lv_display_get_default();
    const int32_t disp_w = d ? lv_display_get_horizontal_resolution(d) : 1280;
    return disp_w - USER_SIDEBAR_W;
}
int32_t col_gutter(void) { return content_w() * 32 / 1152; }   /**< 稿 32：左右内距 */
int32_t col_gap(void)    { return content_w() * 64 / 1152; }   /**< 稿 64：两栏间距 */
int32_t detail_top(void) { return content_w() * 136 / 1152; }  /**< 稿 136：右栏内容顶 */
int32_t preview_h(void)  { return content_w() * 322 / 1152; }  /**< 稿 322：预览框高 */
int32_t btn_h(void)      { return content_w() * 72 / 1152; }   /**< 稿 72：按钮高 */
int32_t detail_bot(void) { return content_w() * 32 / 1152; }   /**< 稿 800-768 = 32：按钮组底边 */
int32_t btn_gap(void)    { return content_w() * 16 / 1152; }   /**< 稿 16：删除 / 确认打印并排的缝 */

/* ---- 前置声明 ---- */
void rebuild_file_grid(void);
void update_detail_panel(const control_file_t *file);
void highlight_file_rows(void);
/* 定义在详情面板那一段，但文件列表行也要用（make_file_card 在其之前） */
void make_detail_inline_kv(lv_obj_t *parent, const lv_image_dsc_t *icon,
                           const char *caption,
                           lv_color_t value_color, lv_obj_t **out_val,
                           const lv_font_t *value_font = nullptr);

/* 文件大小（字节）→ 人类可读字符串 */
void format_file_size(char *buf, size_t n, uint32_t bytes)
{
    if(bytes >= 1024u * 1024u) {
        std::snprintf(buf, n, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else if(bytes >= 1024u) {
        std::snprintf(buf, n, "%.1f KB", (double)bytes / 1024.0);
    } else {
        std::snprintf(buf, n, "%u B", bytes);
    }
}

/* 选中文件 → 填充右侧详情面板。
 * 本机：预估耗时/耗材 + 删除按钮（原样）；USB：仅文件大小，无次按钮。 */
void update_detail_panel(const control_file_t *file)
{
    const bool has = (file != nullptr);
    if(g_detail_hint) {
        if(has) lv_obj_add_flag(g_detail_hint, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_clear_flag(g_detail_hint, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_detail_body) {
        if(has) lv_obj_clear_flag(g_detail_body, LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(g_detail_body, LV_OBJ_FLAG_HIDDEN);
    }
    if(!has) {
        return;
    }

    const bool is_usb = (g_file_tab == FileTab::Usb);

    if(g_detail_name_lbl) {
        lv_label_set_text(g_detail_name_lbl, file->name);
    }

    if(g_detail_preview_img && g_detail_preview_icon) {
        const bool show_real_thumb = !is_usb && ui_user_thumb_path_exists(file->thumb_large);
        if(show_real_thumb) {
            lv_image_set_src(g_detail_preview_img, file->thumb_large_src);
            lv_obj_clear_flag(g_detail_preview_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(g_detail_preview_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_image_set_src(g_detail_preview_img, nullptr);
            lv_obj_add_flag(g_detail_preview_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(g_detail_preview_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* meta 行：本机显示耗时/耗材，USB 显示文件大小（互斥） */
    if(g_detail_meta_local) {
        if(is_usb) lv_obj_add_flag(g_detail_meta_local, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(g_detail_meta_local, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_detail_meta_usb) {
        if(is_usb) lv_obj_clear_flag(g_detail_meta_usb, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(g_detail_meta_usb, LV_OBJ_FLAG_HIDDEN);
    }
    if(!is_usb) {
        if(g_detail_time_lbl) {
            char dur[32];
            ui_user_format_duration_min(dur, sizeof(dur), file->duration_min);
            lv_label_set_text(g_detail_time_lbl, dur);
        }
        if(g_detail_mat_lbl) {
            lv_label_set_text_fmt(g_detail_mat_lbl, "%.1f g", (double)file->material_g);
        }
    } else if(g_detail_size_lbl) {
        char sz[32];
        format_file_size(sz, sizeof(sz), file->file_size);
        lv_label_set_text(g_detail_size_lbl, sz);
    }

    /* 次按钮：仅本机显示“删除该文件”，USB 无次按钮（确认打印随之铺满整行） */
    if(g_detail_secondary_btn) {
        if(is_usb) lv_obj_add_flag(g_detail_secondary_btn, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_clear_flag(g_detail_secondary_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if(g_detail_secondary_lbl) {
        lv_label_set_text(g_detail_secondary_lbl, "Delete File");
    }
}

void on_file_card_click(lv_event_t *e)
{
    const size_t index =
        static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    const control_file_t *file = control_files_get(index);
    if(!file || !g_files_page_active) {
        return;
    }
    /* 再次点击已选中的文件 → 取消选中，回到空态 */
    if(g_sel_index == static_cast<int>(index)) {
        g_sel_index = -1;
        update_detail_panel(nullptr);
        highlight_file_rows();
        return;
    }
    g_sel_index = static_cast<int>(index);
    control_files_set_active(file);
    update_detail_panel(file);
    highlight_file_rows();
    /* 不再在选择时请求 file_info：详情用目录项数据即可，避免后端为预览而全量解析阻塞 */
}

void on_file_select_cancel(lv_event_t * /*e*/)
{
    /* 选择文件页的返回键：回到进来时的分区（首页仪表盘或监控页） */
    control_files_close();
}

void on_print_blocked_dismiss(lv_event_t * /*e*/)
{
    control_files_close_dialog();
}

/** 弹窗内直接下发复位；成败/在途由消息中心与结果页的三态钮体现 */
void on_print_blocked_reset(lv_event_t * /*e*/)
{
    control_files_close_dialog();
    const ui_alarm_entry_t *blk = ui_alarm_print_blocker();
    if(blk) {
        ui_alarm_request_reset(blk->code);
    }
}

/* 详情面板「Start Print」→ Prepare → D1… */
void on_detail_start_print(lv_event_t * /*e*/)
{
    if(!g_files_page_active || g_sel_index < 0) {
        return;
    }
    /* 顺手请求一次快照：本地阻断状态可能已经过期（单条上报错过不补发），
     * 拿旧状态拦下一次本可以开始的打印比较冤。请求是异步的，这次预检仍用当前
     * 缓存判断——真有偏差，下次点击就纠正过来了。 */
    ui_alarm_request_snapshot("print start");

    /* 阻断报警预检：与其等 MCU 拒绝，进确认弹窗前就把原因亮出来。
     * clear_mode=RESET 给「Reset」直达；其余（如断电重启）只能知会。 */
    if(const ui_alarm_entry_t *blk = ui_alarm_print_blocker()) {
        char body[256];
        std::snprintf(body, sizeof(body), "%s\n%s", blk->title, blk->body);
        /* 与消息页抽屉、结果页失败条同一判据（见 ui_alarm_reset_allowed） */
        const bool resettable = ui_alarm_reset_allowed(blk);
        control_files_open_confirm("Unable to start print", body,
                                   "Cancel", resettable ? "Reset" : "OK",
                                   user_color_status_err, true,
                                   on_print_blocked_dismiss,
                                   resettable ? on_print_blocked_reset
                                              : on_print_blocked_dismiss);
        return;
    }
    control_files_show_print_confirm();
}

void on_delete_cancel(lv_event_t * /*e*/)
{
    control_files_close_dialog();
}

void on_delete_confirm(lv_event_t * /*e*/)
{
    control_files_close_dialog();
    if(!control_files_delete_allowed() || g_sel_index < 0 ||
       g_file_tab != FileTab::Local) {
        return;
    }
    const uint32_t idx = static_cast<uint32_t>(g_sel_index);
    ui::UiMsgProxy::instance().send_fs_delete_ntf(idx, middleware::PrintFileSource::Local);
    g_sel_index = -1;
    control_files_set_active(nullptr);
    update_detail_panel(nullptr);
}

/* 删除二次确认弹窗（稿 2567:5555）：标题问句 + 一句不可撤销说明 + 取消/删除 */
void show_delete_confirm(void)
{
    /* 文件页 IDLE 时 g_overlay 隐藏，须挂 top layer（同 control_terminate_request） */
    control_files_open_confirm("Delete this file?", "This cannot be undone.",
                               "Cancel", "Delete", user_color_status_err, true,
                               on_delete_cancel, on_delete_confirm);
}

/* 详情面板底部次按钮：仅本机显示 → 删除该文件（二次确认） */
void on_detail_secondary(lv_event_t * /*e*/)
{
    if(!control_files_delete_allowed() || g_sel_index < 0 ||
       g_file_tab != FileTab::Local) {
        return;
    }
    show_delete_confirm();
}

/* 移动滑动胶囊高亮块到当前源，并刷新两个文字的明暗 */
void seg_set_active(FileTab tab, bool animate)
{
    if(g_seg_pill) {
        const int32_t pill_w = lv_obj_get_width(g_seg_pill);
        /* 内距 2 + 段宽 + 段间 4：第二段的左沿要多让 4（稿 266 / 394） */
        const int32_t target = (tab == FileTab::Local) ? 0 : (pill_w + 4);
        if(animate) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, g_seg_pill);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
            lv_anim_set_values(&a, lv_obj_get_x(g_seg_pill), target);
            lv_anim_set_duration(&a, 180);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        } else {
            lv_obj_set_x(g_seg_pill, target);
        }
    }
    /* 稿上选中与否靠字重（SemiBold / Medium）分层，Montserrat 单字重追不了，
     * 只落地不透明度那一半——同消息页 Tab 的处理。 */
    if(g_seg_lbl_local) {
        lv_obj_set_style_text_opa(g_seg_lbl_local,
            tab == FileTab::Local ? LV_OPA_COVER : LV_OPA_50, 0);
    }
    if(g_seg_lbl_usb) {
        lv_obj_set_style_text_opa(g_seg_lbl_usb,
            tab == FileTab::Usb ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}

void switch_source(FileTab tab)
{
    if(g_file_tab == tab) {
        return;
    }
    g_file_tab = tab;
    g_sel_index = -1;
    seg_set_active(tab, true);
    update_detail_panel(nullptr);
    control_files_clear();
    rebuild_file_grid();
    control_files_request_dir_open(tab);
}

void on_tab_local(lv_event_t * /*e*/) { switch_source(FileTab::Local); }

void on_tab_usb(lv_event_t * /*e*/)   { switch_source(FileTab::Usb); }

/**
 * 文件列表行（稿 6240:93093）：512×104 的白卡，内距 16，
 * gap 24 排 [缩略图 64] [名称 24 / 指标 18]。
 *
 * 选中态是**整块换成实色 `#EBEAEC`**（稿 6240:93110），不是在白底上叠一层——
 * 叠色会把下面的白透出来，出不来稿上那个灰度。见 highlight_file_rows()。
 */
lv_obj_t *make_file_card(lv_obj_t *parent, const control_file_t *file, size_t index)
{
    const int32_t thumb_sz = USER_PAD_LG * 2;   /* 稿 64 */

    lv_obj_t *row = lv_button_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, user_light_card, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, USER_HIST_CARD_RADIUS, 0);   /* 稿 16 */
    lv_obj_set_style_shadow_width(row, 6, 0);
    lv_obj_set_style_shadow_ofs_y(row, 2, 0);
    lv_obj_set_style_shadow_color(row, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(row, 10, 0);                  /* 稿 4% */
    lv_obj_set_style_pad_all(row, USER_PAD_SM, 0);            /* 稿 16 */
    lv_obj_set_style_pad_column(row, USER_PAD, 0);            /* 稿 24 */
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, on_file_card_click, LV_EVENT_SHORT_CLICKED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(index)));

    /* 缩略图：64 见方、圆角 8、底 rgba(51,49,69,.05)；无图时出文件占位图标 */
    lv_obj_t *thumb = lv_obj_create(row);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_size(thumb, thumb_sz, thumb_sz);
    lv_obj_set_style_bg_color(thumb, user_light_text, 0);
    lv_obj_set_style_bg_opa(thumb, 13, 0);
    lv_obj_set_style_radius(thumb, USER_BTN_RADIUS, 0);       /* 稿 8 */
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
    const bool show_real_thumb =
        (g_file_tab != FileTab::Usb) && ui_user_thumb_path_exists(file->thumb_small);
    if(show_real_thumb) {
        lv_obj_t *img = lv_image_create(thumb);
        lv_image_set_src(img, file->thumb_small_src);
        lv_obj_set_size(img, thumb_sz, thumb_sz);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, 0);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_COVER);
    } else {
        lv_obj_t *ticon = lv_label_create(thumb);
        lv_label_set_text(ticon, LV_SYMBOL_FILE);
        lv_obj_set_style_text_color(ticon, user_light_text, 0);
        lv_obj_set_style_text_opa(ticon, LV_OPA_40, 0);
        lv_obj_set_style_text_font(ticon, user_font_title(), 0);
        lv_obj_center(ticon);
    }

    /* 文字列：名称 + meta，稿上行距 12 */
    lv_obj_t *col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, USER_PAD_SM * 3 / 4, 0);    /* 稿 12 */
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(col);
    lv_label_set_text(name, file->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_text_color(name, user_light_text, 0);
    lv_obj_set_style_text_font(name, user_font_body(), 0);    /* 稿 24 */

    /* USB 列表行不显示预估耗时/耗材（仅本机显示）。
     * 稿上是 [图标16 + 文字18]，项间 16、图标↔文字 8，无 caption。 */
    if(g_file_tab != FileTab::Usb) {
        lv_obj_t *meta = lv_obj_create(col);
        lv_obj_remove_style_all(meta);
        lv_obj_set_size(meta, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(meta, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(meta, USER_PAD_SM, 0);    /* 稿 16 */
        lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(meta, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(meta, LV_OBJ_FLAG_CLICKABLE);

        char dur[32];
        ui_user_format_duration_min(dur, sizeof(dur), file->duration_min);
        lv_obj_t *dur_val = nullptr;
        make_detail_inline_kv(meta, &ic_clock, nullptr, user_light_text,
                              &dur_val, user_font_text());
        if(dur_val) {
            lv_label_set_text(dur_val, dur);
        }

        lv_obj_t *mat_val = nullptr;
        make_detail_inline_kv(meta, &ic_cube, nullptr, user_light_text,
                              &mat_val, user_font_text());
        if(mat_val) {
            lv_label_set_text_fmt(mat_val, "%.1fg",
                                  static_cast<double>(file->material_g));
        }
    }

    return row;
}

void highlight_file_rows(void)
{
    if(!g_file_grid) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(g_file_grid);
    for(uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(g_file_grid, i);
        const bool sel = (static_cast<int>(i) == g_sel_index);
        /* 稿 6240:93110：选中行整块换成实色 #EBEAEC，其余一切照旧（阴影/圆角不变）。
         * 不用叠色——白卡上叠半透明灰会把底透出来，出不来稿上那个灰度。 */
        lv_obj_set_style_bg_color(row, sel ? user_light_row_sel : user_light_card, 0);
    }
}

void rebuild_file_grid(void)
{
    if(!g_file_grid) {
        return;
    }
    /* 保存重建前的滚动位置，重建后恢复（切回页面刷新目录时不跳回顶端） */
    const int32_t prev_scroll_y = lv_obj_get_scroll_y(g_file_grid);

    lv_obj_clean(g_file_grid);
    const size_t n = control_files_count();
    if(n == 0) {
        /* 空态在列表区里居中，与右栏的 g_detail_hint 对称。标签自己纵向居中不了
         * （g_file_grid 是 FLEX_ALIGN_START 的列），套一层吃满剩余高度的容器来做。
         * 底部让出 detail_bot()，让这句与右栏那句落在同一条水平线上——两栏的可用
         * 高度差正是右栏那 32 的底内距。
         *
         * U 盘那档单独给话术：稿上没画这态，但「没插盘」和「插了没文件」是两回事，
         * 一句 No print files 会让人以为盘是空的。 */
        lv_obj_t *box = lv_obj_create(g_file_grid);
        lv_obj_remove_style_all(box);
        lv_obj_set_width(box, LV_PCT(100));
        lv_obj_set_flex_grow(box, 1);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_bottom(box, detail_bot(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *empty = lv_label_create(box);
        lv_label_set_text(empty, g_file_tab == FileTab::Usb ? "No USB drive"
                                                            : "No print files available");
        lv_obj_set_style_text_color(empty, user_light_text, 0);
        lv_obj_set_style_text_opa(empty, LV_OPA_50, 0);
        lv_obj_set_style_text_font(empty, user_font_title(), 0);
        return;
    }
    for(size_t i = 0; i < n; i++) {
        const control_file_t *file = control_files_get(i);
        if(file) {
            make_file_card(g_file_grid, file, i);
        }
    }
    highlight_file_rows();

    /* 先让布局/滚动范围生效，再恢复滚动位置（超出范围会被自动钳制） */
    lv_obj_update_layout(g_file_grid);
    lv_obj_scroll_to_y(g_file_grid, prev_scroll_y, LV_ANIM_OFF);
}

/**
 * 本机 / USB 页签（稿 6240:93091）。
 *
 * Figma 组件「切换Tab」（6156:82872）：256×72、圆角 16、底 `rgba(51,49,69,.05)`、内距 2、
 * 段间 4，选中段白底圆角 14 加一道 `0 2 6 rgba(12,28,97,.04)`。旧版设置页的 Wi-Fi 开关分段
 * 也是它；设置页新稿换成了开关，语言分段是另一套样式（深色选中块），两处不再需要同步。
 */
lv_obj_t *make_source_segment(lv_obj_t *parent)
{
    const int32_t seg_h = USER_SETTINGS_SEG_H > 0 ? USER_SETTINGS_SEG_H
                                                  : USER_NAV_ITEM_H * 4 / 5;
    const int32_t seg_w = USER_NAV_ITEM_H * 16 / 5;   /* 行高 80 → 正好稿的 256 */

    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_remove_style_all(track);
    lv_obj_set_size(track, seg_w, seg_h);
    lv_obj_set_style_radius(track, USER_HIST_CARD_RADIUS, 0);   /* 稿 16 */
    lv_obj_set_style_bg_color(track, user_light_text, 0);
    lv_obj_set_style_bg_opa(track, 13, 0);                      /* 稿 5% */
    lv_obj_set_style_pad_all(track, 2, 0);
    lv_obj_set_flex_flow(track, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t pill_w = (seg_w - 4 - 4) / 2;   /* 去掉内距 2×2 与段间 4 */

    /* 浮动滑块（不参与 flex，绝对定位、可动画） */
    g_seg_pill = lv_obj_create(track);
    lv_obj_remove_style_all(g_seg_pill);
    lv_obj_add_flag(g_seg_pill, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(g_seg_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_seg_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_seg_pill, pill_w, seg_h - 4);
    lv_obj_set_pos(g_seg_pill, 0, 0);
    lv_obj_set_style_radius(g_seg_pill, USER_HIST_CARD_RADIUS - 2, 0);  /* 稿 14，与外框同心 */
    lv_obj_set_style_bg_color(g_seg_pill, user_light_card, 0);
    lv_obj_set_style_bg_opa(g_seg_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(g_seg_pill, 6, 0);
    lv_obj_set_style_shadow_ofs_y(g_seg_pill, 2, 0);
    lv_obj_set_style_shadow_color(g_seg_pill, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(g_seg_pill, 10, 0);

    /* 两个等宽透明点击按钮（压在滑块之上） */
    lv_obj_t *btn_local = lv_button_create(track);
    lv_obj_remove_style_all(btn_local);
    lv_obj_set_size(btn_local, pill_w, LV_PCT(100));
    lv_obj_set_style_bg_opa(btn_local, LV_OPA_TRANSP, 0);
    lv_obj_set_style_margin_right(btn_local, 4, 0);   /* 稿 段间 4 */
    lv_obj_add_event_cb(btn_local, on_tab_local, LV_EVENT_CLICKED, nullptr);
    g_seg_lbl_local = lv_label_create(btn_local);
    lv_label_set_text(g_seg_lbl_local, "Local");
    lv_obj_set_style_text_font(g_seg_lbl_local, user_font_title(), 0);   /* 稿 20 */
    lv_obj_set_style_text_color(g_seg_lbl_local, user_light_text, 0);
    lv_obj_center(g_seg_lbl_local);

    lv_obj_t *btn_usb = lv_button_create(track);
    lv_obj_remove_style_all(btn_usb);
    lv_obj_set_size(btn_usb, pill_w, LV_PCT(100));
    lv_obj_set_style_bg_opa(btn_usb, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(btn_usb, on_tab_usb, LV_EVENT_CLICKED, nullptr);
    g_seg_lbl_usb = lv_label_create(btn_usb);
    lv_label_set_text(g_seg_lbl_usb, "USB");
    lv_obj_set_style_text_font(g_seg_lbl_usb, user_font_title(), 0);
    lv_obj_set_style_text_color(g_seg_lbl_usb, user_light_text, 0);
    lv_obj_center(g_seg_lbl_usb);

    seg_set_active(g_file_tab, false);
    return track;
}

/* 「[图标] caption value」行内小项，详情面板与文件列表行共用。
 * （稿 4014:16631 把原先的两行 caption/value 压成一行。）
 *
 * icon       传 nullptr 不占图标位（USB 的文件大小行）
 * caption    传 nullptr 不建标签，也就不会多吃一份 pad_column（列表行只要图标+数值）
 * value_font 传 nullptr 用 user_font_text()（列表行传 caption 字号以维持行高）
 * out_val    回传 value 句柄以便后续刷新 */
void make_detail_inline_kv(lv_obj_t *parent, const lv_image_dsc_t *icon,
                           const char *caption,
                           lv_color_t value_color, lv_obj_t **out_val,
                           const lv_font_t *value_font)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, USER_PAD_SM, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* A8 图标按 24px 原尺寸 1:1 贴（见 gen_home_metric_icons.py 的 clock/cube 档）。
     * **别在这儿加缩放**：lv_draw_sw_transform 是 2 抽头点采样、不做面积平均，缩小时
     * 会整行整列跳过源像素，而这两枚图标的描边只有 1.33px，跳掉一行就断一截，实机上
     * 是肉眼可见的锯齿。真需要小一号就照 METRIC_ICONS 的做法让生成器另出一档原生
     * 尺寸（同 ic_home_26/32/48），运行时始终 1:1。 */
    if(icon) {
        lv_obj_t *img = lv_image_create(row);
        lv_image_set_src(img, icon);
        lv_obj_set_style_image_recolor(img, user_light_text, 0);
        lv_obj_set_style_image_opa(img, LV_OPA_50, 0);
        lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    }

    if(caption) {
        lv_obj_t *cap = lv_label_create(row);
        lv_label_set_text(cap, caption);
        lv_obj_set_style_text_color(cap, user_light_text, 0);
        lv_obj_set_style_text_opa(cap, LV_OPA_50, 0);
        /* caption 与 value 同号（稿 6240:93059/93060 都是 16），靠不透明度分层 */
        lv_obj_set_style_text_font(cap, value_font ? value_font : user_font_text(), 0);
    }

    lv_obj_t *val = lv_label_create(row);
    /* 占位符统一用 ASCII "--"：内置 Montserrat 只收 0x20-0x7F,0xB0,0x2022，
     * 破折号 U+2014 不在范围内，会渲染成豆腐块 */
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, value_color, 0);
    lv_obj_set_style_text_font(val, value_font ? value_font : user_font_text(), 0);
    if(out_val) {
        *out_val = val;
    }
}

/**
 * 右栏详情（稿 6240:93047，按钮组按 6357:34689）：512×632 的 column space-between。
 *
 * 上组贴顶 = 预览框（322 圆角 24）+ gap16 + 信息（名称 24 / 指标 16 带标签）；
 * 下组贴底 = 按钮组。空态只是把上下两组藏起来、露出居中的提示语。
 */
void build_detail_panel(lv_obj_t *parent)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, LV_PCT(100));
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 未选文件时的占位提示。稿上是左对齐，这里按产品决定改成在右栏内水平居中。 */
    g_detail_hint = lv_obj_create(panel);
    lv_obj_remove_style_all(g_detail_hint);
    lv_obj_set_width(g_detail_hint, LV_PCT(100));
    lv_obj_set_flex_grow(g_detail_hint, 1);
    lv_obj_set_flex_flow(g_detail_hint, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_detail_hint, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_detail_hint, USER_PAD_SM / 2, 0);
    lv_obj_set_style_bg_opa(g_detail_hint, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_detail_hint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *hint_t = lv_label_create(g_detail_hint);
    lv_label_set_text(hint_t, "No file selected");
    lv_obj_set_style_text_color(hint_t, user_light_text, 0);
    lv_obj_set_style_text_font(hint_t, user_font_body(), 0);
    lv_obj_t *hint_b = lv_label_create(g_detail_hint);
    lv_label_set_text(hint_b, "Pick a file on the left to continue");
    lv_obj_set_style_text_color(hint_b, user_light_text, 0);
    lv_obj_set_style_text_opa(hint_b, LV_OPA_50, 0);
    lv_obj_set_style_text_font(hint_b, user_font_title(), 0);

    /* 选中后显示的详情内容（上组 + 按钮组撑满整栏） */
    g_detail_body = lv_obj_create(panel);
    lv_obj_remove_style_all(g_detail_body);
    lv_obj_set_width(g_detail_body, LV_PCT(100));
    lv_obj_set_flex_grow(g_detail_body, 1);
    lv_obj_set_flex_flow(g_detail_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_detail_body, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_bg_opa(g_detail_body, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_detail_body, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 上组：预览 + 名称 + 指标 ---- */
    lv_obj_t *top = lv_obj_create(g_detail_body);
    lv_obj_remove_style_all(top);
    lv_obj_set_width(top, LV_PCT(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(top, USER_PAD_SM, 0);          /* 稿 16 */
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    /* 大预览：稿 512×322、圆角 24、底 rgba(51,49,69,.15) */
    lv_obj_t *preview = lv_obj_create(top);
    lv_obj_remove_style_all(preview);
    lv_obj_set_width(preview, LV_PCT(100));
    lv_obj_set_height(preview, preview_h());
    lv_obj_set_style_bg_color(preview, user_light_text, 0);
    lv_obj_set_style_bg_opa(preview, 38, 0);                /* 稿 15% */
    lv_obj_set_style_radius(preview, USER_LIGHT_RADIUS, 0); /* 稿 24 */
    lv_obj_set_style_clip_corner(preview, true, 0);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
    g_detail_preview_img = lv_image_create(preview);
    lv_image_set_src(g_detail_preview_img, nullptr);
    lv_obj_set_size(g_detail_preview_img, LV_PCT(100), LV_PCT(100));
    lv_obj_align(g_detail_preview_img, LV_ALIGN_CENTER, 0, 0);
    /* 320×180 进稿 512×322：CONTAIN 上下会露出框底灰边，COVER 铺满。 */
    lv_image_set_inner_align(g_detail_preview_img, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(g_detail_preview_img, LV_OBJ_FLAG_HIDDEN);

    /* 没有缩略图时的占位（USB 文件通常都没有） */
    g_detail_preview_icon = lv_label_create(preview);
    lv_label_set_text(g_detail_preview_icon, LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(g_detail_preview_icon, user_light_text, 0);
    lv_obj_set_style_text_opa(g_detail_preview_icon, LV_OPA_40, 0);
    lv_obj_set_style_text_font(g_detail_preview_icon, user_font_value(), 0);
    lv_obj_center(g_detail_preview_icon);

    /* 文件名（稿 24） */
    g_detail_name_lbl = lv_label_create(top);
    lv_label_set_text(g_detail_name_lbl, "--");
    lv_label_set_long_mode(g_detail_name_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(g_detail_name_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(g_detail_name_lbl, user_light_text, 0);
    lv_obj_set_style_text_font(g_detail_name_lbl, user_font_body(), 0);

    /* 本机 meta：耗时 / 耗材 一行，稿上项间 32、字号 16、带标签 */
    g_detail_meta_local = lv_obj_create(top);
    lv_obj_remove_style_all(g_detail_meta_local);
    lv_obj_set_width(g_detail_meta_local, LV_PCT(100));
    lv_obj_set_height(g_detail_meta_local, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_detail_meta_local, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g_detail_meta_local, USER_PAD_LG, 0);   /* 稿 32 */
    lv_obj_set_style_bg_opa(g_detail_meta_local, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_detail_meta_local, LV_OBJ_FLAG_SCROLLABLE);
    make_detail_inline_kv(g_detail_meta_local, &ic_clock, "Est. time",
                          user_light_text, &g_detail_time_lbl, user_font_text_sm());
    make_detail_inline_kv(g_detail_meta_local, &ic_cube, "Est. material",
                          user_light_text, &g_detail_mat_lbl, user_font_text_sm());

    /* USB meta：文件大小（与本机 meta 互斥显示） */
    g_detail_meta_usb = lv_obj_create(top);
    lv_obj_remove_style_all(g_detail_meta_usb);
    lv_obj_set_width(g_detail_meta_usb, LV_PCT(100));
    lv_obj_set_height(g_detail_meta_usb, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g_detail_meta_usb, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_bg_opa(g_detail_meta_usb, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_detail_meta_usb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_detail_meta_usb, LV_OBJ_FLAG_HIDDEN);
    make_detail_inline_kv(g_detail_meta_usb, nullptr, "File size",
                          user_light_text, &g_detail_size_lbl, user_font_text_sm());

    /* ---- 下组：按钮，贴右栏底 ----
     *
     * 稿 6357:34689：删除在左、确认打印在右，同一行各 248×72、缝 16、圆角 16，底边 768。
     * 两枚都 flex_grow 均分整行：U 盘文件不能删、删除钮隐藏时，确认打印自然铺满 512
     * （产品定的，稿只画了本机文件那一态）。 */
    lv_obj_t *btns = lv_obj_create(g_detail_body);
    lv_obj_remove_style_all(btns);
    lv_obj_set_width(btns, LV_PCT(100));
    lv_obj_set_height(btns, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, btn_gap(), 0);       /* 稿 16 */
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(btns, LV_OBJ_FLAG_SCROLLABLE);

    /* 次按钮（左）：仅本机显示「删除该文件」（USB 在 update_detail_panel 隐藏）。
     * 稿是 1px #FF4242 描边、无底色、红字 22；旧稿那版是实心红，新稿改成了描边。 */
    g_detail_secondary_btn = lv_button_create(btns);
    lv_obj_t *sec = g_detail_secondary_btn;
    lv_obj_remove_style_all(sec);
    lv_obj_set_flex_grow(sec, 1);
    lv_obj_set_height(sec, btn_h());
    lv_obj_set_style_radius(sec, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_border_width(sec, 1, 0);
    lv_obj_set_style_border_color(sec, user_color_status_err, 0);
    lv_obj_set_style_bg_color(sec, user_color_status_err, 0);
    lv_obj_set_style_bg_opa(sec, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(sec, 26, LV_STATE_PRESSED);    /* 按下铺一层 10% 红 */
    lv_obj_clear_flag(sec, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(sec, on_detail_secondary, LV_EVENT_CLICKED, nullptr);

    g_detail_secondary_lbl = lv_label_create(sec);
    lv_label_set_text(g_detail_secondary_lbl, "Delete File");
    lv_obj_set_style_text_color(g_detail_secondary_lbl, user_color_status_err, 0);
    lv_obj_set_style_text_font(g_detail_secondary_lbl, user_font_dlg_btn(), 0);   /* 稿 22 */
    lv_obj_center(g_detail_secondary_lbl);

    /* 主按钮（右）：稿 248×72、圆角 16、实底 #333145、文字 22 白 */
    g_detail_start_btn = lv_button_create(btns);
    lv_obj_t *btn_start = g_detail_start_btn;
    lv_obj_remove_style_all(btn_start);
    lv_obj_set_flex_grow(btn_start, 1);
    lv_obj_set_height(btn_start, btn_h());
    lv_obj_set_style_radius(btn_start, USER_HIST_CARD_RADIUS, 0);
    lv_obj_set_style_bg_color(btn_start, user_light_text, 0);
    lv_obj_set_style_bg_opa(btn_start, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(btn_start, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_clear_flag(btn_start, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_start, on_detail_start_print, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(btn_start);
    lv_label_set_text(bl, "Confirm Print");
    lv_obj_set_style_text_color(bl, user_light_card, 0);
    lv_obj_set_style_text_font(bl, user_font_dlg_btn(), 0);   /* 稿 22 */
    lv_obj_center(bl);
}

/**
 * 两栏之间那道竖分隔线（稿 6240:93043，整屏 x=704、通高）。
 *
 * 它正落在两栏的中线上，而 flex 的列间距是“空的”、挂不住东西，所以做成脱离布局的
 * FLOATING 子节点、按父容器居中对齐。稿给的是 0.5px + `rgba(0,0,0,.1)`，LVGL 最细
 * 只有 1px，改用半档不透明度把视觉重量压回去。
 */
void build_column_divider(lv_obj_t *parent)
{
    lv_obj_t *vline = lv_obj_create(parent);
    lv_obj_remove_style_all(vline);
    /* 必须用 IGNORE_LAYOUT 而不是 FLOATING：后者虽不参与排布，却仍被算进 flex 的
     * pad_column，两栏会各被挤掉半个列间距（实测 512 变 480）。 */
    lv_obj_add_flag(vline, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(vline, 1, LV_PCT(100));
    lv_obj_set_style_bg_color(vline, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vline, 13, 0);        /* 稿 10% 的一半，抵掉 1px 比 0.5px 粗 */
    lv_obj_remove_flag(vline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(vline, LV_ALIGN_CENTER, 0, 0);
}

/** 返回钮（稿 6240:93044）：72 见方、圆角 24 的白卡，图标 48。 */
lv_obj_t *make_round_icon_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb)
{
    const int32_t sz = USER_LIGHT_BTN_SZ;   /* 稿 72，与消息页抽屉返回钮同档 */

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, USER_LIGHT_RADIUS, 0);   /* 稿 24 */
    lv_obj_set_style_bg_color(btn, user_light_card, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, user_light_more_bg, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 5, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 1, 0);
    lv_obj_set_style_shadow_color(btn, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(btn, 10, 0);              /* 稿 4% */
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if(cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    }
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, user_light_text, 0);
    lv_obj_set_style_text_font(icon, user_font_body(), 0);
    lv_obj_center(icon);
    return btn;
}

/* 选择文件页内容：建入 PageManager 内容区（parent 已在侧栏右侧），填满整块。
 * 返回页根，供 page_user_files_create 交给 PageManager 托管。 */
lv_obj_t *show_file_select(lv_obj_t *parent)
{
    /* 稿 6240:92891 的两栏在内容区 1152 里正好闭合，两栏等宽：
     *   左内距 32 + 左栏 512 + 列间距 64 + 右栏 512 + 右内距 32
     * 分隔线落在两栏正中（整屏 x=704），故不能把列间距塞进 pad_column——
     * 那样线就没地方挂了，见下面 build_column_divider()。 */
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, user_light_bg, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_file_panel = page;

    /* 两栏装在自己的 flex 行里，分隔线作为 page 的另一个子节点浮在上面。
     * **分隔线不能进这个 flex 行**：即便带 IGNORE_LAYOUT，LVGL 仍会按 3 个子项
     * 算 2 道列间距，两栏各被挤掉 32（实测 512 变 480）。 */
    lv_obj_t *cols = lv_obj_create(page);
    lv_obj_remove_style_all(cols);
    lv_obj_set_size(cols, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_hor(cols, col_gutter(), 0);
    lv_obj_set_style_pad_column(cols, col_gap(), 0);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(cols, LV_OBJ_FLAG_SCROLLABLE);

    build_column_divider(page);

    /* ============ 左栏：顶部 返回钮 + 页签；下方 文件列表 ============ */
    lv_obj_t *left = lv_obj_create(cols);
    lv_obj_remove_style_all(left);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_grow(left, 1);           /* 两栏等宽，稿上都是 512 */
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(left, USER_PAD_LG, 0);      /* 稿 32 */
    lv_obj_set_style_pad_row(left, USER_PAD_LG, 0);      /* 稿 顶部行↔列表 32 */
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    /* 左栏顶部：返回钮 + 本机/USB 页签（稿 6240:93044 / 93091） */
    lv_obj_t *lhead = lv_obj_create(left);
    lv_obj_remove_style_all(lhead);
    lv_obj_set_width(lhead, LV_PCT(100));
    lv_obj_set_height(lhead, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lhead, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(lhead, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(lhead, USER_PAD_LG, 0);  /* 稿 264-232 = 32 */
    lv_obj_set_style_bg_opa(lhead, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(lhead, LV_OBJ_FLAG_SCROLLABLE);
    make_round_icon_btn(lhead, LV_SYMBOL_LEFT, on_file_select_cancel);
    make_source_segment(lhead);

    /* 文件列表（纵向行，可滚动） */
    g_file_grid = lv_obj_create(left);
    lv_obj_remove_style_all(g_file_grid);
    lv_obj_set_width(g_file_grid, LV_PCT(100));
    lv_obj_set_flex_grow(g_file_grid, 1);
    lv_obj_set_flex_flow(g_file_grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_file_grid, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(g_file_grid, USER_PAD_SM, 0);   /* 稿 卡间距 16 */
    lv_obj_set_style_bg_opa(g_file_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(g_file_grid, LV_SCROLLBAR_MODE_OFF);
    rebuild_file_grid();

    /* ============ 右栏：预览 + 信息 + 按钮组（含未选时的空态） ============
     *
     * 稿 6240:93047 是 x736 y136、column space-between：上组（预览 + 名称 + 指标）贴顶，
     * 按钮组贴底（6357:34689 起按钮组是一行、底边 768，栏高 512×632）。中间那两枚上下
     * 翻页圆钮与顶部的供应度条**新稿都没画**，已随稿去掉；两栏之间的分隔线保留
     * （见 build_column_divider）。 */
    lv_obj_t *right = lv_obj_create(cols);
    lv_obj_remove_style_all(right);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(right, detail_top(), 0);       /* 稿 136 */
    lv_obj_set_style_pad_bottom(right, detail_bot(), 0);    /* 稿 800-768 = 32 */
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    build_detail_panel(right);
    update_detail_panel(nullptr);

    return page;
}

}  /* namespace */

/* ============================================================================
 *  对外接口（声明见 page_user_files.hpp）
 * ========================================================================= */

/* ---- 选择文件页（PageManager 隐藏页 "user_files"）生命周期 ---- */
lv_obj_t *page_user_files_create(lv_obj_t *parent)
{
    /* 仅首次创建时复位到本机/未选；之后切走再回来由 PageManager 隐藏/显示保留状态 */
    g_file_tab = FileTab::Local;
    g_sel_index = -1;
    return show_file_select(parent);
}

void page_user_files_refresh(bool /*force*/)
{
    /* on_enter：标记可见；Local 读 config.json，USB 走 Fs_DirOpen；选中项按名恢复 */
    g_files_page_active = true;
    control_files_reset_dir_pending();
    control_files_request_dir_open(g_file_tab);
}

void page_user_files_exit(void)
{
    g_files_page_active = false;
    control_files_reset_dir_pending();
    control_files_close_dialog();
}

void page_user_files_rebuild(void)
{
    rebuild_file_grid();
    update_detail_panel(g_sel_index >= 0 ? control_files_get(
                            static_cast<size_t>(g_sel_index)) : nullptr);
}

void page_user_files_set_selection(int index)
{
    g_sel_index = index;
}

void page_user_files_reset(void)
{
    g_file_tab  = FileTab::Local;
    g_sel_index = -1;
    /* 控件可能还没建（会话复位早于首次进页），seg_set_active 内部按空指针短路 */
    seg_set_active(g_file_tab, false);
}

int     page_user_files_sel_index(void) { return g_sel_index; }
FileTab page_user_files_source(void)    { return g_file_tab; }
bool    page_user_files_active(void)    { return g_files_page_active; }
