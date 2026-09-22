#ifndef PAGE_MANAGER_HPP
#define PAGE_MANAGER_HPP

#include "lvgl_wrap.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace ui {

/**
 * @brief 页面基类
 * 
 * 所有具体页面都需要继承此类，并重写相应的虚函数以实现页面生命周期管理。
 * 生命周期顺序：on_create → on_enter → on_exit → on_destroy
 */
class Page {
public:
    virtual ~Page() = default;

    /** @return 页面唯一标识名称 */
    virtual std::string name() const = 0;

    /** 
     * @brief 页面创建回调
     * @param parent 父容器对象（通常为右侧内容区 content_panel）
     * 
     * 在此函数中创建页面的所有 LVGL 控件，仅在页面首次显示时调用一次。
     */
    virtual void on_create(lv_obj_t* parent) = 0;

    /** 
     * @brief 页面进入回调
     * 
     * 每次切换到该页面时调用，可用于恢复页面状态、刷新数据等。
     */
    virtual void on_enter() {}

    /**
     * @brief 页面退出回调
     * 
     * 每次从该页面切换到其他页面时调用，可用于保存状态、停止动画等。
     */
    virtual void on_exit() {}
};

using PagePtr = std::unique_ptr<Page>;

/**
 * @brief 页面管理器（单例）
 * 
 * 负责管理所有页面的注册、切换和生命周期。
 * 采用左侧图标菜单 + 右侧内容区的经典布局。
 */
class PageManager {
public:
    /** @return 页面管理器单例实例 */
    static PageManager& instance();

    /** 
     * @brief 初始化页面管理器
     * @param screen 顶层屏幕对象
     * 
     * 创建左侧菜单栏（menu_panel_）和右侧内容区（content_panel_），
     * 并设置好 flex 布局。
     */
    void init(lv_obj_t* screen);

    /** 
     * @brief 注册页面
     * @param page 页面智能指针，所有权转移至管理器
     * @param menu_icon 菜单按钮上显示的 LVGL 符号，如 LV_SYMBOL_HOME；若为 nullptr 则显示页面名称
     */
    void register_page(PagePtr page, const char* menu_icon);

    /**
     * @brief 注册页面（使用图片图标，推荐：可还原设计稿自定义图标）
     * @param page          页面智能指针
     * @param menu_icon_set A8 图标的多档 set（NULL 结尾），如 `ic_nav_home_set`
     * @param badge_icon_set 可选：**有未读红点时**换上的那版图标（NULL = 没有变体）
     *
     * 收 set 而不是单张 dsc：图标按档位各出一份原生尺寸，这里按 `USER_NAV_ICON_SZ`
     * 挑等宽的那份、上屏 1:1 不缩放（挑图与「为什么不缩放」见 `ui_user_pick_icon()`）。
     * 选中与否只差颜色，同一枚图标 recolor 即可——未选中 `user_light_icon_off`、选中白，
     * 底板由按钮自身的 `LV_STATE_CHECKED` 画，见 `make_menu_button()`。
     *
     * `badge_icon_set` 是另一回事，**不是颜色能替代的**：稿上带红点那版把图标右上角
     * 咬掉了一块给红点让位（消息页 6255:95805，与首页铃铛 6201:85994 同一套做法），
     * 轮廓本身就不一样。给了它，`set_menu_badge()` 会连图标一起换；不给就只叠红点，
     * 红点会压在原轮廓上。
     */
    void register_page(PagePtr page, const lv_image_dsc_t* const* menu_icon_set,
                       const lv_image_dsc_t* const* badge_icon_set = nullptr);

    /**
     * @brief 注册「隐藏页」：有页面、但不在侧边栏生成菜单按钮。
     *
     * 用于非顶层导航目的地（如从首页「My Files」进入的选择文件页）：
     * 只能通过 switch_to(name) 编程切换，享受 PageManager 的创建一次/隐藏显示/状态保留。
     *
     * @param owner_menu 归属菜单页名：该隐藏页激活时，用此页的菜单按钮表示高亮
     *                   （如选择文件页归属 "user_home"，使首页图标保持高亮）。空=不映射。
     */
    void register_hidden_page(PagePtr page, std::string owner_menu = std::string());

    /**
     * @brief 为某菜单页设置「动态目标解析器」。
     *
     * 设置后，点击该页的侧边栏按钮不再固定切到本页，而是切到 resolver() 返回的页名。
     * 用于把首页图标做成「首页区」入口：返回首页区当前视图（仪表盘 / 选择文件页）。
     */
    void set_menu_target_resolver(const std::string& page_name,
                                  std::function<std::string()> resolver);

    /** 
     * @brief 切换到指定页面（非栈式切换）
     * @param name 页面名称
     * 
     * 直接跳转目标页面，不维护导航栈。当前页面会先执行 on_exit，
     * 目标页面执行 on_create（首次）和 on_enter。
     */
    void switch_to(const std::string& name);

    /**
     * @brief 在某菜单按钮的图标右上角开/关一颗红点角标。
     * @param page_name 目标页名；无菜单按钮的隐藏页忽略
     * @param on        true=亮起，false=熄灭（对象保留，只切显隐）
     *
     * 目前只有消息页在用（未读数 >0 时亮）。首次 on 时才建对象，
     * 从没亮过的页面不会平白多一个子节点。
     */
    void set_menu_badge(const std::string& page_name, bool on);

    /**
     * @brief 侧栏切到「整屏视频」配色（首页打印中 / 监控中）。
     *
     * 浅色稿把侧栏改成了几乎全透的白 4% + 一道右侧外投影，压在 #f5f5f5 页面上很轻。
     * 但整屏摄像头预览时侧栏背后是视频帧，那点不透明度压不住亮帧，`#9E9E9E` 的未选中
     * 图标会被盖过去——这个坑 app/ui/UI.md「外壳布局」一节记过，当年 bg_opa 从 40%
     * 提到 90% 就是为它。所以这两个态退回深底 + 原来那档图标灰。
     */
    void set_sidebar_video_mode(bool on);

    /**
     * @brief 整屏底色切浅色档（`user_light_bg`）还是深色档（`user_color_bg`）。
     *
     * 底色画在 `shell_` 上，`content_panel_` 与各页根节点都是透明的，所以这一个开关
     * 决定所有页面的底。**按页设**：浅色稿是一页一页改过来的，已改的页开、没改的页
     * 仍是深色内容、翻成浅底会立刻看不见字。名单在 `apply_device_bar()` 里。
     *
     * 换页时由 `apply_device_bar()` 统一置位，页面自己一般不用调。
     */
    void set_shell_light(bool on);

    /**
     * @brief 把侧栏重新提到 shell_ 的最前。
     *
     * 侧栏在 `init()` 末尾被提到最前，是外壳里 z 序最高的一层。通知面板展开时要临时
     * 越过它（捕获层得盖住侧栏，否则点侧栏收不起面板、会直接切页），收起后由
     * `ui_notice_stack` 调本函数把次序还原。
     */
    void raise_sidebar();

private:
    PageManager() = default;

    /** @brief 内部页面条目，关联 Page 对象与其对应的菜单按钮 */
    struct PageEntry {
        PagePtr page;           /**< 页面实例 */
        lv_obj_t* menu_btn = nullptr; /**< 左侧菜单按钮对象 */
        /** A8 图标，靠 recolor 换色：未选中灰、选中白（浅色稿 6073:72801 里选中态是
         *  深色底板 + 白字形，不再是早先那种渐变字形，故一层就够）。 */
        lv_obj_t* menu_icon_img = nullptr;
        lv_obj_t* menu_badge = nullptr;    /**< 未读红点（懒建，见 set_menu_badge） */
        /** 常态 / 带红点两版图标 set；后者为 NULL 表示这枚图标没有变体。 */
        const lv_image_dsc_t* const* menu_icon_set = nullptr;
        const lv_image_dsc_t* const* menu_icon_badge_set = nullptr;
        std::string owner_menu;  /**< 隐藏页：激活时用此页的菜单按钮高亮（空=无映射） */
        std::function<std::string()> target_resolver; /**< 菜单按钮点击的动态目标（空=切到本页） */
        bool created = false;   /**< 标记页面是否已执行过 on_create */
    };

    lv_obj_t* screen_ = nullptr;       /**< 顶层屏幕 */
    lv_obj_t* shell_ = nullptr;        /**< 根容器，包含菜单栏和内容区 */
    lv_obj_t* menu_panel_ = nullptr;   /**< 左侧菜单栏容器 */
    lv_obj_t* content_panel_ = nullptr;/**< 右侧内容区容器 */
    lv_obj_t* device_bar_ = nullptr;   /**< 常驻设备信息条（浮于页面之上） */
    lv_obj_t* notice_stack_ = nullptr; /**< 左下通知浮层（浮于页面之上） */
    std::vector<PageEntry> entries_;   /**< 已注册页面列表 */
    Page* current_ = nullptr;          /**< 当前显示的页面 */
    bool sidebar_video_ = false;       /**< 侧栏是否处于整屏视频配色，见 set_sidebar_video_mode */
    bool shell_light_ = false;         /**< 整屏底是否浅色档，见 set_shell_light */

    /** 按 sidebar_video_ 刷一遍侧栏底与图标色。两处调用：切换视频态、重新高亮。 */
    void apply_sidebar_style();

    /** 按 shell_light_ 刷一遍整屏底色。两处调用：init() 建壳、切换档位。 */
    void apply_shell_style();

    /** 根据名称查找对应的页面条目 */
    PageEntry* find_entry(const std::string& name);

    /**
     * 按目标页显隐常驻设备信息条与左下通知浮层，并同步 content_panel_ 的顶部让位。
     * 规则：侧栏有菜单按钮的页面显示，隐藏页（选文件 / 调试 / 延时摄影）不显示，
     * 不显示时把 pad_top 收回 0，页面拿回整屏高度。
     * 通知浮层额外排除消息页自己。
     */
    void apply_device_bar(const PageEntry& entry);

    /** 菜单按钮点击事件的静态回调函数 */
    static void menu_btn_event_cb(lv_event_t* e);

    /** 根据当前活动页面名称，高亮对应的菜单按钮 */
    void update_menu_highlight(const std::string& active_name);
};

} // namespace ui

#endif
