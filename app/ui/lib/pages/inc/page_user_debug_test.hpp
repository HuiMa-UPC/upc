#ifndef PAGE_USER_DEBUG_TEST_H
#define PAGE_USER_DEBUG_TEST_H

#include "lvgl_wrap.hpp"
#include "upc_contract.hpp"


/**
 * @file page_user_debug_test.hpp
 * @brief 调试页「测试」Tab —— 自动化测试用例表。
 *
 * 任务表按行顺序执行，可设置每行参数、运行时间和完成后的延迟时间，并按设定循环次数循环。
 * 由 lv_timer 驱动的状态机推进，命令经 DebugMsgProxy 下发；等待类用轴位置回读做闭环。
 * 本模块只依赖 UI 侧接口（DebugMsgProxy），不直接触碰后端/协议。
 */

namespace ui {

/** 构建「测试」Tab 内容容器（初始隐藏，由 page_user_debug 的 Tab 切换控制显隐）。 */
lv_obj_t *build_debug_test_tab(lv_obj_t *body);

/** 由 page_user_debug 的 register_axis 回调转发的实时轴位置，供“等待到位”判断使用。 */
void debug_test_feed_axis(const middleware::DebugAxisStatePayload &p);

/** 停止正在运行的测试流程（退页时调用），必要时补发点动停止。 */
void debug_test_stop_flow(void);

/** 复位模块状态（页面重建前调用）：停止流程、清空对象指针缓存。 */
void debug_test_reset(void);

} // namespace ui

#endif
