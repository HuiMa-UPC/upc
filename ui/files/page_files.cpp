#include "page_files.hpp"
#include "files_open.hpp"
#include "gcode_worker.hpp"
#include "ctrl_backend.hpp"
#include "ui_theme.hpp"
#include "ui_feedback.hpp"
#include "job_status.hpp"

#include <cstring>
#include <cstdio>

/* ---------- UI widgets ---------- */
static lv_obj_t *g_list = nullptr;
static lv_obj_t *g_lbl_name = nullptr;
static lv_obj_t *g_lbl_size = nullptr;
static lv_obj_t *g_lbl_lines = nullptr;
static lv_obj_t *g_lbl_status = nullptr;
static lv_obj_t *g_lbl_ring_full = nullptr;
static lv_obj_t *g_bar_parse = nullptr;
static lv_obj_t *g_btn_open = nullptr;
static lv_obj_t *g_open_lbl = nullptr;
static lv_obj_t *g_detail = nullptr;
static lv_timer_t *g_ui_timer = nullptr;
static bool g_page_visible = false;

static gcode_file_list_t g_file_list;
static int g_selected_idx = -1;
/** File bound to ring buffer / parse worker (-1 = none) */
static int g_active_idx = -1;
/** File currently parsing (-1 = parse idle) */
static int g_parsing_idx = -1;

typedef enum {
    FILE_SLOT_IDLE = 0,
    FILE_SLOT_PARSING,
    FILE_SLOT_READY,
    FILE_SLOT_FAILED,
} file_slot_state_t;

typedef struct {
    file_slot_state_t state;
    int total_lines;
    int parsed_lines;
    int packets;
} file_slot_t;

static file_slot_t g_slots[GCODE_MAX_FILES];

/** 重置单个文件槽位到初始状态 */
static void reset_file_slot(int idx)
{
    if(idx < 0 || idx >= GCODE_MAX_FILES) {
        return;
    }
    g_slots[idx].state        = FILE_SLOT_IDLE;
    g_slots[idx].total_lines  = 0;
    g_slots[idx].parsed_lines = 0;
    g_slots[idx].packets      = 0;
}

/** 重置所有文件槽位 */
static void reset_all_slots(void)
{
    for(int i = 0; i < GCODE_MAX_FILES; i++) {
        reset_file_slot(i);
    }
}

/** 将字节数格式化为人类可读字符串（B / KB / MB） */
static void format_size(long size, char *out, size_t out_size)
{
    if(size < 1024) {
        snprintf(out, out_size, "%ld B", size);
    } else if(size < 1024 * 1024) {
        snprintf(out, out_size, "%.2f KB", size / 1024.0);
    } else {
        snprintf(out, out_size, "%.2f MB", size / (1024.0 * 1024.0));
    }
}

/** 设置文件列表按钮的选中/未选中视觉样式 */
static void set_btn_selected_style(lv_obj_t *btn, bool selected)
{
    lv_obj_set_style_bg_color(btn,
        selected ? ui_color_accent_dim : ui_color_panel_alt, 0);
    lv_obj_set_style_border_width(btn, selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(btn,
        selected ? ui_color_accent : ui_color_border, 0);
}

/** 启用或禁用 Open 按钮，并同步切换视觉状态 */
static void set_open_btn_enabled(bool enabled)
{
    if(!g_btn_open) {
        return;
    }
    if(enabled) {
        lv_obj_remove_state(g_btn_open, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(g_btn_open, ui_color_accent_dim, 0);
    } else {
        lv_obj_add_state(g_btn_open, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(g_btn_open, ui_color_panel_alt, 0);
    }
}

/** 修改 Open 按钮的文本标签 */
static void set_open_btn_caption(const char *caption)
{
    if(g_open_lbl && caption) {
        lv_label_set_text(g_open_lbl, caption);
    }
}

/**
 * @brief 根据当前选中项和发送状态更新 Open 按钮的可用性与文案
 *
 * - 未选中文件：禁用，显示 "Open"
 * - 正在发送：禁用，显示 "Locked"
 * - 当前选中的文件正在解析：显示 "Opening"
 * - 其他：显示 "Open"
 */
static void update_open_button(void)
{
    if(g_selected_idx < 0 || g_selected_idx >= g_file_list.count) {
        set_open_btn_enabled(false);
        set_open_btn_caption(LV_SYMBOL_PLAY "  Open");
        return;
    }

    if(ctrl_is_sending()) {
        set_open_btn_enabled(false);
        set_open_btn_caption(LV_SYMBOL_STOP "  Locked");
        return;
    }

    set_open_btn_enabled(true);
    if(g_parsing_idx == g_selected_idx) {
        set_open_btn_caption(LV_SYMBOL_LOOP "  Opening");
    } else {
        set_open_btn_caption(LV_SYMBOL_PLAY "  Open");
    }
}

/**
 * @brief 刷新右侧详情面板（仅根据内存状态，不涉及磁盘 I/O）
 *
 * 根据 g_selected_idx 对应的文件信息和槽位状态，更新：
 * - 文件名、大小、行数
 * - 解析进度条与百分比
 * - 状态文本（Parsing / Ready / Failed / Selected）
 * - Open 按钮状态
 */
static void refresh_detail_panel(void)
{
    const int idx = g_selected_idx;
    if(idx < 0 || idx >= g_file_list.count) {
        lv_label_set_text(g_lbl_name, "No file selected");
        lv_label_set_text(g_lbl_size, "Size: --");
        lv_label_set_text(g_lbl_lines, "G-code lines: --");
        lv_bar_set_value(g_bar_parse, 0, LV_ANIM_OFF);
        ui_status_set(g_lbl_status, "Select a file and tap Open", ui_color_text_dim);
        if(g_lbl_ring_full) {
            lv_obj_add_flag(g_lbl_ring_full, LV_OBJ_FLAG_HIDDEN);
        }
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 0, 0);
        }
        update_open_button();
        return;
    }

    const gcode_file_info_t *info = &g_file_list.files[idx];
    const file_slot_t *slot = &g_slots[idx];
    char line_buf[64];
    char size_raw[48];
    char size_line[64];
    char name_line[280];
    char status_buf[192];

    snprintf(name_line, sizeof(name_line), "File: %s", info->name);
    lv_label_set_text(g_lbl_name, name_line);

    format_size(info->size, size_raw, sizeof(size_raw));
    snprintf(size_line, sizeof(size_line), "Size: %s", size_raw);
    lv_label_set_text(g_lbl_size, size_line);

    if(slot->total_lines > 0) {
        snprintf(line_buf, sizeof(line_buf), "G-code lines: %d", slot->total_lines);
    } else {
        snprintf(line_buf, sizeof(line_buf), "G-code lines: --");
    }
    lv_label_set_text(g_lbl_lines, line_buf);

    int pct = 0;
    lv_color_t status_color = ui_color_text_dim;
    bool ring_full = false;
    {
        JOB_LOCK();
        ring_full = g_job_status.ring_full;
    }

    const bool is_parsing_this =
        (g_parsing_idx == idx && gcode_is_parsing()) ||
        (slot->state == FILE_SLOT_PARSING && g_active_idx == idx);

    if(g_lbl_ring_full) {
        if(is_parsing_this && ring_full) {
            lv_obj_remove_flag(g_lbl_ring_full, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(g_lbl_ring_full, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if(is_parsing_this && slot->total_lines > 0) {
        pct = (slot->parsed_lines * 100) / slot->total_lines;
        if(pct > 100) {
            pct = 100;
        }
        if(ring_full) {
            snprintf(status_buf, sizeof(status_buf),
                     LV_SYMBOL_PAUSE " Parsing: %d%% (%d/%d lines, %d pkts) — "
                     "ring queue full, waiting for space",
                     pct, slot->parsed_lines, slot->total_lines, slot->packets);
            status_color = ui_color_text_dim;
        } else {
            snprintf(status_buf, sizeof(status_buf),
                     LV_SYMBOL_LOOP " Parsing: %d%% (%d/%d lines, %d pkts)",
                     pct, slot->parsed_lines, slot->total_lines, slot->packets);
            status_color = ui_color_text;
        }
        if(g_bar_parse) {
            lv_obj_set_style_bg_color(g_bar_parse, ui_color_accent, LV_PART_INDICATOR);
        }
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 1, 0);
            lv_obj_set_style_border_color(g_detail, ui_color_border, 0);
        }
    } else if(slot->state == FILE_SLOT_READY) {
        if(g_bar_parse) {
            lv_obj_set_style_bg_color(g_bar_parse, ui_color_accent, LV_PART_INDICATOR);
        }
        pct = 100;
        snprintf(status_buf, sizeof(status_buf),
                 LV_SYMBOL_OK " Ready: %d packets, %d lines",
                 slot->packets, slot->total_lines);
        status_color = ui_color_accent;
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 2, 0);
            lv_obj_set_style_border_color(g_detail, ui_color_accent, 0);
        }
    } else if(slot->state == FILE_SLOT_FAILED) {
        snprintf(status_buf, sizeof(status_buf),
                 LV_SYMBOL_WARNING " Open failed — tap Open to retry");
        status_color = ui_color_danger;
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 0, 0);
        }
    } else if(g_parsing_idx >= 0 && g_parsing_idx < g_file_list.count &&
              g_parsing_idx != idx) {
        snprintf(status_buf, sizeof(status_buf),
                 "Background: parsing %s — tap Open here to switch",
                 g_file_list.files[g_parsing_idx].name);
        status_color = ui_color_text_dim;
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 0, 0);
        }
    } else {
        snprintf(status_buf, sizeof(status_buf), "Selected — tap Open to load");
        if(g_detail) {
            lv_obj_set_style_border_width(g_detail, 0, 0);
        }
    }

    lv_bar_set_value(g_bar_parse, pct, LV_ANIM_OFF);
    ui_status_set(g_lbl_status, status_buf, status_color);
    update_open_button();
}

/** 遍历文件列表，同步按钮选中样式与 g_selected_idx */
static void update_list_selection(void)
{
    const uint32_t cnt = lv_obj_get_child_count(g_list);
    for(uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(g_list, i);
        if(!lv_obj_check_type(btn, &lv_button_class)) {
            continue;
        }
        set_btn_selected_style(btn, (int)i == g_selected_idx);
    }
}

/** 文件列表项点击事件：更新选中索引并刷新详情面板 */
static void on_file_btn_click(lv_event_t *e)
{
    const int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if(idx < 0 || idx >= g_file_list.count) {
        return;
    }
    g_selected_idx = idx;
    update_list_selection();
    refresh_detail_panel();
}

/**
 * @brief 停止当前正在运行的作业
 *
 * 依次调用：停止发送 → 关闭文件/停止解析 → 清空 RingBuffer → 重置 parsing 索引
 */
static void stop_active_job(void)
{
    if(ctrl_is_sending()) {
        ctrl_stop_sending();
    }
    gcode_close_file();
    g_ring_buffer.clear();
    g_parsing_idx = -1;
}

/**
 * @brief Refresh 按钮点击事件
 *
 * 停止当前作业，重新扫描 /home/Gcode 目录，重建文件列表 UI。
 */
static void on_refresh_click(lv_event_t *)
{
    stop_active_job();
    g_active_idx = -1;
    reset_all_slots();

    lv_obj_clean(g_list);
    galva_scan_files(&g_file_list);

    if(g_file_list.count == 0) {
        lv_obj_t *lbl = lv_label_create(g_list);
        lv_label_set_text(lbl, "No G-code files in /home/Gcode");
        lv_obj_set_style_text_color(lbl, ui_color_text_dim, 0);
        g_selected_idx = -1;
        refresh_detail_panel();
        return;
    }

    for(int i = 0; i < g_file_list.count; i++) {
        lv_obj_t *btn = lv_button_create(g_list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 38);
        lv_obj_add_event_cb(btn, on_file_btn_click, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_set_style_radius(btn, UI_RADIUS, 0);
        lv_obj_set_style_pad_all(btn, UI_PAD_SM, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn, 8, 0);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, LV_SYMBOL_FILE);
        lv_obj_set_style_text_font(icon, ui_font_icon(), 0);
        lv_obj_set_style_text_color(icon, ui_color_accent, 0);
        lv_obj_set_width(icon, 28);

        lv_obj_t *name = lv_label_create(btn);
        lv_label_set_text(name, g_file_list.files[i].name);
        lv_obj_set_style_text_color(name, ui_color_text, 0);
        lv_obj_set_flex_grow(name, 1);
    }

    g_selected_idx = 0;
    update_list_selection();
    refresh_detail_panel();
}

/**
 * @brief 从解析 worker 同步进度到当前 parsing 槽位
 *
 * 由 UI 定时器调用，用于刷新进度条和状态文本。
 */
static void sync_parsing_slot_from_worker(void)
{
    if(g_parsing_idx < 0 || g_parsing_idx >= g_file_list.count) {
        return;
    }

    file_slot_t *slot = &g_slots[g_parsing_idx];
    slot->state        = FILE_SLOT_PARSING;
    slot->total_lines  = gcode_get_total_lines();
    slot->parsed_lines = gcode_get_parsed_lines();

    {
        JOB_LOCK();
        slot->packets = g_job_status.total_packets;
    }
}

/**
 * @brief 被动检测并挂接外部重启的解析线程
 *
 * 当 Control 页触发 Reset 并重新调用 gcode_start_parse 时，
 * Files 页可能未收到通知，此函数在定时器中检测并自动挂接进度。
 */
static void files_attach_active_parse_if_needed(void)
{
    if(g_parsing_idx >= 0 || !gcode_is_parsing()) {
        return;
    }
    if(g_active_idx < 0 || g_active_idx >= g_file_list.count) {
        return;
    }

    g_parsing_idx = g_active_idx;
    file_slot_t *slot = &g_slots[g_active_idx];
    slot->state = FILE_SLOT_PARSING;
    slot->parsed_lines = gcode_get_parsed_lines();
    {
        JOB_LOCK();
        slot->packets = g_job_status.total_packets;
    }
}

/**
 * @brief 外部通知：解析已被重启，Files 页需重新挂接进度
 *
 * 由 ui_shell 或 Control 页在 Reset 后调用，将当前 active 文件槽位置为 PARSING 并重置计数器。
 */
void page_files_notify_parse_restarted(void)
{
    if(g_active_idx < 0 || g_active_idx >= g_file_list.count) {
        return;
    }

    g_parsing_idx = g_active_idx;
    file_slot_t *slot = &g_slots[g_active_idx];
    slot->state        = FILE_SLOT_PARSING;
    slot->parsed_lines = 0;
    slot->packets      = 0;
    slot->total_lines  = gcode_get_total_lines();

    if(g_page_visible && g_selected_idx == g_active_idx) {
        refresh_detail_panel();
    }
}

/**
 * @brief Files 页 UI 定时器回调（约 120ms 周期）
 *
 * 职责：
 * 1. 更新 RingBuffer 水位
 * 2. 被动挂接外部重启的解析线程
 * 3. 同步解析进度并刷新 UI
 * 4. 检测解析完成，切换槽位状态并弹出 toast
 */
static void files_ui_timer_cb(lv_timer_t *)
{
    job_status_update_ring();
    files_attach_active_parse_if_needed();

    if(g_parsing_idx >= 0 && g_parsing_idx < g_file_list.count) {
        sync_parsing_slot_from_worker();

        if(gcode_is_parsing()) {
            if(g_page_visible && g_selected_idx == g_parsing_idx) {
                refresh_detail_panel();
            }
            return;
        }

        /* Parse finished */
        file_slot_t *slot = &g_slots[g_parsing_idx];
        const int finished_idx = g_parsing_idx;

        if(slot->total_lines <= 0) {
            slot->state = FILE_SLOT_FAILED;
            g_active_idx = -1;
        } else {
            slot->state = FILE_SLOT_READY;
            g_active_idx = finished_idx;
        }
        g_parsing_idx = -1;

        {
            JOB_LOCK();
            g_job_status.is_parsing = false;
        }

        if(slot->state == FILE_SLOT_READY) {
            ui_toast_show("G-code loaded — go to Control to Start", ui_color_accent, 2200);
        } else {
            ui_toast_show("Failed to open G-code file", ui_color_danger, 2500);
        }
        if(g_selected_idx == finished_idx) {
            refresh_detail_panel();
        }
        return;
    }

    if(g_page_visible && g_selected_idx >= 0) {
        refresh_detail_panel();
    } else if(g_selected_idx >= 0) {
        update_open_button();
    }
}

/**
 * @brief Files 页显示/隐藏切换
 * @param visible 1 显示，0 隐藏
 *
 * 隐藏时停止刷新进度条，避免切走页面后状态错乱。
 */
void page_files_set_visible(int visible)
{
    g_page_visible = (visible != 0);
    if(g_page_visible) {
        sync_parsing_slot_from_worker();
        refresh_detail_panel();
    }
}

/**
 * @brief 打开选中的 G-code 文件并启动后台解析线程
 *
 * 完整链路：
 *   1. 合法性检查（是否选中文件、是否正在发送）
 *   2. 组装完整文件路径（/home/Gcode/xxx.gcode）
 *   3. 清理其他文件槽位的解析状态
 *   4. 停止当前任务、重置当前槽位
 *   5. 标记当前槽位为 "解析中"
 *   6. 打开文件并预统计总行数（gcode_open_file）
 *   7. 清零全局作业状态（total_packets / sent_packets / parsed_lines）
 *   8. 启动解析线程（gcode_start_parse），逐行读取写入 g_ring_buffer
 */
static void begin_open_selected_file(void)
{
    /* 1. 检查是否已选中文件 */
    if(g_selected_idx < 0 || g_selected_idx >= g_file_list.count) {
        ui_toast_show("Please select a file first", ui_color_danger, 1800);
        return;
    }

    /* 2. 若当前正在向下位机发送，禁止切换文件 */
    if(ctrl_is_sending()) {
        ui_toast_show("Stop the job before switching G-code", ui_color_danger, 2500);
        refresh_detail_panel();
        return;
    }

    const int idx = g_selected_idx;
    char path[512];
    galva_build_full_path(g_file_list.files[idx].name, path, sizeof(path));

    /* 3. 切换/重新解析时：清理非当前选中文件的槽位状态，避免残留 */
    if(g_active_idx >= 0 && g_active_idx < g_file_list.count && g_active_idx != idx) {
        reset_file_slot(g_active_idx);
    }
    if(g_parsing_idx >= 0 && g_parsing_idx < g_file_list.count && g_parsing_idx != idx) {
        reset_file_slot(g_parsing_idx);
    }

    /* 4. 停止正在运行的后台任务（解析线程、发送线程），并清空 RingBuffer */
    stop_active_job();
    /* 重置当前槽位状态（清除旧的解析进度、失败标记等） */
    reset_file_slot(idx);

    /* 5. 标记当前文件槽位进入 "解析中" 状态 */
    g_slots[idx].state = FILE_SLOT_PARSING;
    g_parsing_idx = idx;
    g_active_idx = idx;

    /* 6. 打开 G-code 文件；gcode_open_file 内部会 fopen 并 fgets 预统计总行数 */
    if(gcode_open_file(path) != 0) {
        g_slots[idx].state = FILE_SLOT_FAILED;
        g_parsing_idx = -1;
        g_active_idx = -1;
        ui_toast_show("Cannot open file", ui_color_danger, 2500);
        refresh_detail_panel();
        return;
    }

    /* 记录文件总行数，用于 UI 进度展示 */
    g_slots[idx].total_lines = gcode_get_total_lines();

    /* 7. 清零全局作业计数器，准备新一轮解析/发送 */
    {
        JOB_LOCK();
        g_job_status.total_packets = 0;
        g_job_status.sent_packets = 0;
        g_job_status.parsed_lines = 0;
        g_job_status.total_lines = g_slots[idx].total_lines;
        g_job_status.is_parsing = true;
    }

    /* 8. 启动后台解析线程：逐行读取 → 解析 G0/G1/M3000 → 写入 g_ring_buffer */
    gcode_start_parse(&g_ring_buffer);
    refresh_detail_panel();
}

/** Open 按钮点击事件：打开选中文件并启动解析 */
static void on_open_click(lv_event_t *)
{
    begin_open_selected_file();
}

/**
 * @brief 创建 Files 页面
 * @param parent LVGL 父对象
 * @return 页面根对象
 *
 * 页面布局：
 * - 顶部标题栏 + Refresh 按钮
 * - 左侧 38%：文件列表
 * - 右侧 62%：文件详情面板（名称、大小、行数、进度条、状态、Open 按钮）
 */
lv_obj_t *page_files_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, UI_PAD, 0);
    lv_obj_set_style_pad_row(page, UI_PAD, 0);

    lv_obj_t *title_row = lv_obj_create(page);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_width(title_row, LV_PCT(100));
    lv_obj_set_height(title_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, LV_SYMBOL_FILE "  G-code Files");
    lv_obj_add_style(title, ui_style_title(), 0);

    lv_obj_t *btn_refresh = lv_button_create(title_row);
    lv_obj_add_style(btn_refresh, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(btn_refresh, on_refresh_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_lbl = lv_label_create(btn_refresh);
    lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_center(refresh_lbl);

    lv_obj_t *body = lv_obj_create(page);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, UI_PAD, 0);

    lv_obj_t *left_panel = lv_obj_create(body);
    lv_obj_add_style(left_panel, ui_style_panel(), 0);
    lv_obj_set_width(left_panel, LV_PCT(38));
    lv_obj_set_height(left_panel, LV_PCT(100));
    lv_obj_set_flex_flow(left_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(left_panel, UI_PAD_SM, 0);
    lv_obj_set_style_pad_row(left_panel, UI_PAD_SM, 0);

    g_list = lv_obj_create(left_panel);
    lv_obj_remove_style_all(g_list);
    lv_obj_set_width(g_list, LV_PCT(100));
    lv_obj_set_flex_grow(g_list, 1);
    lv_obj_set_flex_flow(g_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_list, UI_PAD_SM, 0);

    g_detail = lv_obj_create(body);
    lv_obj_add_style(g_detail, ui_style_panel(), 0);
    lv_obj_set_flex_grow(g_detail, 1);
    lv_obj_set_height(g_detail, LV_PCT(100));
    lv_obj_set_flex_flow(g_detail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_detail, UI_PAD, 0);
    lv_obj_set_style_pad_row(g_detail, UI_PAD, 0);

    g_lbl_name = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_name, "No file selected");
    lv_obj_add_style(g_lbl_name, ui_style_title(), 0);

    g_lbl_size = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_size, "");
    lv_obj_set_style_text_color(g_lbl_size, ui_color_text_dim, 0);

    g_lbl_lines = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_lines, "");
    lv_obj_set_style_text_color(g_lbl_lines, ui_color_text_dim, 0);

    g_bar_parse = lv_bar_create(g_detail);
    lv_obj_set_width(g_bar_parse, LV_PCT(100));
    lv_obj_set_height(g_bar_parse, 8);
    lv_bar_set_range(g_bar_parse, 0, 100);
    lv_bar_set_value(g_bar_parse, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_bar_parse, ui_color_panel_alt, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_bar_parse, ui_color_accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_bar_parse, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(g_bar_parse, 4, LV_PART_INDICATOR);

    g_lbl_ring_full = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_ring_full,
        LV_SYMBOL_PAUSE " Ring queue full — parsing waits until send frees slots");
    lv_obj_set_style_text_color(g_lbl_ring_full, ui_color_text_dim, 0);
    lv_obj_set_width(g_lbl_ring_full, LV_PCT(100));
    lv_label_set_long_mode(g_lbl_ring_full, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(g_lbl_ring_full, LV_OBJ_FLAG_HIDDEN);

    g_lbl_status = lv_label_create(g_detail);
    lv_label_set_text(g_lbl_status, "Select a file and tap Open to load");
    lv_obj_set_style_text_color(g_lbl_status, ui_color_text_dim, 0);
    lv_label_set_long_mode(g_lbl_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(g_lbl_status, LV_PCT(100));

    lv_obj_t *btn_row = lv_obj_create(g_detail);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, UI_PAD_SM, 0);

    g_btn_open = lv_button_create(btn_row);
    lv_obj_set_size(g_btn_open, 192, UI_INPUT_H);
    lv_obj_add_style(g_btn_open, ui_style_btn_primary(), 0);
    lv_obj_add_event_cb(g_btn_open, on_open_click, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(g_btn_open, ui_color_accent, LV_STATE_PRESSED);
    g_open_lbl = lv_label_create(g_btn_open);
    lv_label_set_text(g_open_lbl, LV_SYMBOL_PLAY "  Open");
    lv_obj_center(g_open_lbl);

    reset_all_slots();
    g_ui_timer = lv_timer_create(files_ui_timer_cb, 120, nullptr);

    on_refresh_click(nullptr);

    return page;
}
