#include "ui_clean_state.hpp"

#include "ui_msg_proxy.hpp"

namespace {

constexpr int kMaxObservers = 4;

ui_clean_state_observer_t g_observers[kMaxObservers] = { nullptr };
bool g_inited = false;

/* 两个 owner 分开存，别合成一个。
 * `g_pending` = 最后一次请求的发起方，`g_active` = 已被后端受理、正在跑的那一路。
 * 合成一个的话：A 在跑、B 又发起 → B 覆盖 owner → 后端拒了 B（正确，REJECTED 报 B）
 * → 可 A 的 DONE 随后会被算到 B 头上，A 的界面永远不复位。 */
ui_clean_owner_t g_pending = UI_CLEAN_OWNER_NONE;
ui_clean_owner_t g_active  = UI_CLEAN_OWNER_NONE;
/** Close 发出时记下的 owner：受理前就点停时 g_active 可能还是 NONE。 */
ui_clean_owner_t g_stop_owner = UI_CLEAN_OWNER_NONE;

/** 跨 `lv_async_call` 传事件用；指针塞不进那个 void* 之外的东西，只能堆上过一手。 */
struct EventCtx {
    ui_clean_event_t ev;
    ui_clean_owner_t owner;
};

void notify_async(void *user)
{
    EventCtx *ctx = static_cast<EventCtx *>(user);
    if(!ctx) {
        return;
    }
    const ui_clean_event_t ev    = ctx->ev;
    const ui_clean_owner_t owner = ctx->owner;
    delete ctx;

    for(int i = 0; i < kMaxObservers; i++) {
        if(g_observers[i]) {
            g_observers[i](ev, owner);
        }
    }
}

/**
 * 把一次回执排到 UI 线程再扇出。
 *
 * Proxy 的 `register_*` 回调跑在总线线程上（见 app/ui/UI.md「UiMsgProxy」一节），
 * 而观察者里会建/毁 LVGL 对象。回跳只在这里做一次，观察者因此不必各自再跳——
 * 这也是把两个页面的 `lv_async_call` 收进本模块的原因之一。
 */
void post(ui_clean_event_t ev, ui_clean_owner_t owner)
{
    lv_async_call(notify_async, new EventCtx{ ev, owner });
}

} // namespace

void ui_clean_state_init(void)
{
    if(g_inited) {
        return;
    }
    g_inited = true;

    auto &proxy = ui::UiMsgProxy::instance();

    proxy.register_clean_test_start_rsp([](bool accepted) {
        if(accepted) {
            g_active = g_pending;
            post(UI_CLEAN_EVT_ACCEPTED, g_active);
        } else {
            /* 只报被拒的那次请求，不动正在跑的那一路 */
            post(UI_CLEAN_EVT_REJECTED, g_pending);
        }
        g_pending = UI_CLEAN_OWNER_NONE;
    });

    proxy.register_clean_test_done([](const middleware::CleanTestDonePayload &rsp) {
        /* 协议约定：result_code 只表示结束类型，00/01/02 都代表流程已结束，
         * UI 一律按正常完成处理，故不读 payload。用户 Close(0xfe) Media 不发本消息。 */
        (void)rsp;
        const ui_clean_owner_t owner = g_active;
        g_active = UI_CLEAN_OWNER_NONE;
        g_stop_owner = UI_CLEAN_OWNER_NONE;
        if(owner != UI_CLEAN_OWNER_NONE) {
            post(UI_CLEAN_EVT_DONE, owner);
        }
    });

    proxy.register_clean_test_close_rsp([]() {
        const ui_clean_owner_t owner =
            (g_active != UI_CLEAN_OWNER_NONE) ? g_active :
            (g_stop_owner != UI_CLEAN_OWNER_NONE) ? g_stop_owner : g_pending;
        g_active = UI_CLEAN_OWNER_NONE;
        g_pending = UI_CLEAN_OWNER_NONE;
        g_stop_owner = UI_CLEAN_OWNER_NONE;
        if(owner != UI_CLEAN_OWNER_NONE) {
            post(UI_CLEAN_EVT_STOPPED, owner);
        }
    });
}

void ui_clean_state_add_observer(ui_clean_state_observer_t cb)
{
    if(!cb) {
        return;
    }
    for(int i = 0; i < kMaxObservers; i++) {
        if(g_observers[i] == cb) {
            return;   /* 幂等：页面重建时重复注册不会叠加 */
        }
        if(!g_observers[i]) {
            g_observers[i] = cb;
            return;
        }
    }
}

void ui_clean_state_request(ui_clean_owner_t owner, const char *job_name)
{
    if(owner == UI_CLEAN_OWNER_NONE) {
        return;
    }
    g_pending = owner;
    g_stop_owner = UI_CLEAN_OWNER_NONE;
    const bool record = (owner == UI_CLEAN_OWNER_PRINT_RESULT);
    /* 宽/高/帧率一律 0 = 跟随 Camera 当前预览参数；两个发起方都没有自己的取值需求。 */
    ui::UiMsgProxy::instance().send_clean_test_start(job_name, 0, 0, 0, record);
}

void ui_clean_state_stop(void)
{
    if(g_active != UI_CLEAN_OWNER_NONE) {
        g_stop_owner = g_active;
    } else if(g_pending != UI_CLEAN_OWNER_NONE) {
        g_stop_owner = g_pending;
    }
    ui::UiMsgProxy::instance().send_clean_test_close();
}

ui_clean_owner_t ui_clean_state_active_owner(void)
{
    return g_active;
}
