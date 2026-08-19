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
     * @param page         页面智能指针
     * @param menu_icon_img A8 图标资源（lv_image_dsc_t）；同时当选中态的位图蒙版用
     *
     * 每个按钮里建两层互斥的图标：未选中露 A8 图标 recolor 成 user_color_icon_off，
     * 选中露一块蓝渐变（稿 2235:8699 的 #4054FD → #4D80EE）经同一份 A8 蒙版裁出的字形。
     * 分两层是因为 image_recolor 只能上单色，画不出稿里的渐变。
     */
    void register_page(PagePtr page, const lv_image_dsc_t* menu_icon_img);

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

private:
    PageManager() = default;

    /** @brief 内部页面条目，关联 Page 对象与其对应的菜单按钮 */
    struct PageEntry {
        PagePtr page;           /**< 页面实例 */
        lv_obj_t* menu_btn = nullptr; /**< 左侧菜单按钮对象 */
        lv_obj_t* menu_icon_img = nullptr; /**< 未选中态：A8 图标 recolor 成灰（图片图标时非空） */
        lv_obj_t* menu_icon_grad = nullptr;/**< 选中态：蓝渐变 + 位图蒙版裁出的字形，与 menu_icon_img 互斥显示 */
        lv_obj_t* menu_badge = nullptr;    /**< 未读红点（懒建，见 set_menu_badge） */
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
