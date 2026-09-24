#ifndef PAGE_USER_FILES_HPP
#define PAGE_USER_FILES_HPP

#include "lvgl_wrap.hpp"
#include "control.hpp"

/**
 * 选择文件页（PageManager 隐藏页 "user_files"）。
 *
 * **本页与 control.cpp 的分工**：这里只管视图——两栏骨架、本机/USB 页签、文件列表、
 * 右侧详情与按钮、删除的二次确认。文件列表数据、打印状态机、开关门与转运盒流程都留在
 * control.cpp，两边经本文件下半部分那几个函数与 control.hpp 里的 `control_files_*`
 * 通信。改动前先读 app/ui/UI.md 的「选文件页」一节。
 *
 * 页面本身由 PageManager 托管、常驻不销毁（切走只隐藏），所以 create 只会跑一次。
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- PageManager 生命周期（注册见 ui_app.cpp）---- */
lv_obj_t *page_user_files_create(lv_obj_t *parent);
void      page_user_files_refresh(bool force);   /**< on_enter：拉目录 + 标记可见 */
void      page_user_files_exit(void);            /**< on_exit：标记不可见 + 关弹窗 */

/* ---- control.cpp → 本页：数据变了，刷视图 ---- */

/** 目录数据已更新，重建列表并按当前选中刷新右栏。列表为空时出空态文案。 */
void page_user_files_rebuild(void);

/** 设置选中项（-1 = 取消选中）。`apply_files_list()` 按文件名跨刷新恢复选中时调用。 */
void page_user_files_set_selection(int index);

/** 会话复位：页签回本机、清选中。`backend_reset_session()` 调用；页面未建也安全。 */
void page_user_files_reset(void);

/* ---- 本页 → control.cpp：当前状态查询 ---- */

int      page_user_files_sel_index(void);   /**< 当前选中下标，-1 = 未选 */
FileTab  page_user_files_source(void);      /**< 当前页签（本机 / USB） */
bool     page_user_files_active(void);      /**< 页面当前是否可见 */

#ifdef __cplusplus
}
#endif

#endif /* PAGE_USER_FILES_HPP */
