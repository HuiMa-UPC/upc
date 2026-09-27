#ifndef CONTROL_H
#define CONTROL_H

#include "lvgl_wrap.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file control.hpp
 * @brief 打印全流程：状态机 + 步骤 overlay UI + UiMsgProxy 后端适配（见 control.cpp）
 */

/* 流程顺序（对齐 Figma「打印前预确认」1.1.x 弹窗序列）：
 *   My Files → PLATE_PROMPT（是否已放入打印版：两卡片入口）
 *     ├─ 已放入 → 选择打印任务 → 选文件页
 *     └─ 未放入 → 滑动开门 → DOOR_OPENING（PM2.5/清粉/开门 loading）→ 选文件页
 *     （调试页 Skip Plate Prompt 打开时不弹 PLATE_PROMPT，直接进选文件页）
 *   选文件页 → Start Print → PREPARE → PRINTING（预览 + Print Start）
 *              → PRINT_DONE（打印结束，预览保活等 Camera 出片）→ 结果页
 *                → 滑动取件 → DOOR_OPENING →〔TRANSFER_BOX 转运盒三步〕→ IDLE
 *                （转运盒三步默认不插入，由调试页开关打开）
 *   开门成功即收 overlay，不再做关门检测/等待。
 */
typedef enum {
    CONTROL_STATE_IDLE = 0,
    CONTROL_STATE_PLATE_PROMPT,     /**< 入口：是否已放入打印版（两卡片 + 已放入/未放入子视图） */
    CONTROL_STATE_DOOR_OPENING,     /**< 未放入分支：PM2.5 检测 + 开门 loading */
    /**
     * 取件分支专用：门开之后的转运盒三步（装盒并抽出隔断板 → 转移工件 → 推入闸门），
     * 走完即回 IDLE。仅在调试开关打开时插入，见 control_set_transfer_box_flow()。
     */
    CONTROL_STATE_TRANSFER_BOX,
    CONTROL_STATE_PREPARE_WAIT,     /**< 选文件后：预处理 loading */
    CONTROL_STATE_PRINTING,         /**< 打印中（预览 + 进度） */
    /**
     * 打印结束（完成或终止）后不直接回 IDLE：摄像头预览保活，界面先维持打印中不变，
     * 等 Camera 的 APP_Print_Video_Manifest_Ntf 到达后切结果页浮层。
     * 当前出口：滑动取件回 IDLE 并走开门流程。
     */
    CONTROL_STATE_PRINT_DONE,
} control_state_t;

typedef struct {
    uint32_t index;
    char     name[64];
    char     path[128];
    char     thumb_small[160];
    char     thumb_large[160];
    char     thumb_print[160];
    char     thumb_small_src[192];
    char     thumb_large_src[192];
    char     thumb_print_src[192];
    uint32_t duration_min;
    float    material_g;
    uint32_t file_size;
    uint32_t line_count;
} control_file_t;

typedef struct {
    int      progress_percent;
    uint32_t remaining_minutes;
    float    pm25_indoor;
    float    pm25_cabin;
    float    temperature_c;
    float    oxygen_percent;
    uint32_t current_layer;  /**< 正在打印的层号（1-based）；total_layers=0 时无意义 */
    uint32_t total_layers;   /**< 总层数；0=后端无法给出（OVF / 无层标记），不显示层号 */
} control_progress_t;

/** 打印结果页展示数据；来源见 control.cpp on_print_video_manifest()。 */
typedef struct {
    bool     terminated;    /**< true=PRINT TERMINATED，false=PRINT COMPLETE */
    bool     has_stats;     /**< manifest 无对应条目（0 帧）时为 false，耗时/耗材显示 "--" */
    char     job_name[64];
    char     video_dir[96]; /**< 录像目录键（通常等于 manifest.frames_dir） */
    uint32_t duration_sec;
    float    material_g;    /**< <0 视为无数据 */
} control_print_result_t;

void control_init(lv_obj_t *parent);

void control_start(void);
void control_cancel(void);
/** 打印占用中（PRINTING 或 PRINT_DONE）：期间禁止进文件页开新任务 */
bool control_is_printing(void);

/* ---- 打印结果页（CONTROL_STATE_PRINT_DONE）的出口 ---- */
/** 滑动取件：退出打印状态后走开门流程（PM2.5/清粉/开门；开完即收） */
void control_print_result_collect(void);

/* 首页/监控「SLIDE TO UNLOCK」独立开门入口：走门弹窗流程（PM2.5/清粉/开门），
 * 开完即回首页。仅 IDLE 态生效。 */
void control_open_door_request(void);

/* ---- 打印中的三个粉料阻塞弹窗（稿 6345:31129 / 6343:30738 / 6343:30854）---- */
typedef enum {
    CONTROL_SUPPLY_DLG_POWDER_EMPTY = 0, /**< 粉舱已空 */
    CONTROL_SUPPLY_DLG_RECYCLE_FULL,     /**< 回粉盒满 */
    CONTROL_SUPPLY_DLG_WASTE_FULL,       /**< 废粉盒满 */
} control_supply_dlg_t;

/**
 * 直接弹出某一档粉料弹窗。**给调试页 Other → Inject 用的联调入口。**
 *
 * 三档里只有 `POWDER_EMPTY` 接了真实触发（`APP_Print_Powder_Low_Ntf`，后端判空并
 * 先自行暂停）；另两档**还没有协议**——MCU 报警表里有 `WASTE_BUCKET_FULL(1101)` 与
 * `SIEVE_PAUSED(2006, 回粉上限接近开关5)`，但都没有接到「打印中阻塞 + 弹窗 + 我已
 * 处理继续」这条路径上。所以先只做 UI，触发靠本函数手动打。
 */
void control_show_supply_dialog(control_supply_dlg_t kind);

/* ---- 选择文件页（page_user_files.cpp）用到的数据与流程入口 ----
 *
 * 页面只做视图，文件列表、打印状态机、确认弹窗都在 control.cpp。反向那几个
 * （页面当前选中/页签）见 page_user_files.hpp。 */

/** 文件来源页签。页面与打印流程都要说这个词，故放在共享头里。 */
enum class FileTab : uint8_t {
    Local = 0,
    Usb   = 1,
};

size_t                control_files_count(void);
const control_file_t *control_files_get(size_t index);

/** 拉取指定来源的目录：本机直读 config.json，USB 走 Fs_DirOpen（应答后回刷页面）。 */
void control_files_request_dir_open(FileTab tab);
/** 清空 USB Fs_DirOpen 的在途队列（进/离页面时调用，避免错位应答刷错 Tab）。 */
void control_files_reset_dir_pending(void);

/** 缓存当前选中文件，供打印流程取用；传 nullptr 清空。 */
void control_files_set_active(const control_file_t *file);
/** 清空文件列表（切页签时先清再拉，避免旧源的条目残留一帧）。 */
void control_files_clear(void);
/** 返回键：首页区视图归位到仪表盘并切页。 */
void control_files_close(void);

/** 弹打印确认（报警预检在页面侧做完之后调用）。 */
void control_files_show_print_confirm(void);

/** 删除是否被允许（打印中不给删）。 */
bool control_files_delete_allowed(void);

/** 通用二次确认弹窗（删除文件 / 终止打印共用）。light=true 走浅色档。 */
void control_files_open_confirm(const char *title, const char *body,
                                const char *cancel_text, const char *confirm_text,
                                lv_color_t confirm_color, bool light,
                                lv_event_cb_t cancel_cb, lv_event_cb_t confirm_cb);
void control_files_close_dialog(void);

const char *control_home_section_target(void);

void control_terminate_request(void);

/* ---- 转运盒流程开关（调试页 Other → Print Gates） ----
 * 打开后，取件分支在门开之后插入转运盒三步；关闭则开门成功即回 IDLE。
 * 纯 UI 本地标志（不发消息、不落盘），重启回到关闭。 */
void control_set_transfer_box_flow(bool on);
bool control_transfer_box_flow_enabled(void);

/* ---- 跳过放版预确认开关（调试页 Other → Print Gates） ----
 * 打开后，control_start() 不弹「是否已放入打印版」，直接进选文件页（等同选「已放入」）。
 * 只管这张入口弹窗；打印准备阶段 Glavo 的放板确认窗归 Skip Plate Present（默认不跳过，读码开着）。
 * 纯 UI 本地标志（不发消息、不落盘），重启回到关闭。 */
void control_set_skip_plate_prompt(bool on);
bool control_skip_plate_prompt_enabled(void);

/* ---- 供应度（耗材/滤芯）：control.cpp 是这份数据的唯一持有者 ----
 * `UiMsgProxy::register_supply_status` 是**单槽**的，页面里千万别再注册一次——
 * 那会静默顶掉 control.cpp 的注册，首页余量胶囊从此不再更新。
 * 要用这份数据请走下面三个接口（同 ui_device_state 的形态：快照 + 观察者扇出）。
 * 选文件页不再拉这条（旧稿顶部 supply bar 已撤）。 */
typedef struct {
    float material_pct;   /**< 耗材余量 0..100 */
    float filter_pct;     /**< 滤芯余量 0..100 */
    bool  material_ok;    /**< true=充足 */
    bool  filter_ok;      /**< true=充足 */
} control_supply_t;

/** 观察者：快照变化后在 UI 线程被调用（Proxy 回调本就在 UI 线程）。 */
typedef void (*control_supply_observer_t)(const control_supply_t *s);

/** 当前快照；指针在进程生命周期内恒定，可长期持有。
 *  后端还没回过 Rsp 时是占位值，进页面时用 control_supply_request() 主动拉一次。 */
const control_supply_t *control_supply_get(void);

/** 请求后端刷新一次；结果经观察者回调，本函数不阻塞也不回调。 */
void control_supply_request(void);

/** 注册观察者（上限 4 个，幂等；注册时不会立即回调）。 */
void control_supply_add_observer(control_supply_observer_t cb);

#ifdef __cplusplus
}
#endif

#endif
