#ifndef PAGE_MANAGER_HPP
#define PAGE_MANAGER_HPP

#include "lvgl_wrap.hpp"
#include <string>
#include <vector>
#include <memory>

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
     * @brief 切换到指定页面（非栈式切换）
     * @param name 页面名称
     * 
     * 直接跳转目标页面，不维护导航栈。当前页面会先执行 on_exit，
     * 目标页面执行 on_create（首次）和 on_enter。
     */
    void switch_to(const std::string& name);

private:
    PageManager() = default;

    /** @brief 内部页面条目，关联 Page 对象与其对应的菜单按钮 */
    struct PageEntry {
        PagePtr page;           /**< 页面实例 */
        lv_obj_t* menu_btn = nullptr; /**< 左侧菜单按钮对象 */
        bool created = false;   /**< 标记页面是否已执行过 on_create */
    };

    lv_obj_t* screen_ = nullptr;       /**< 顶层屏幕 */
    lv_obj_t* shell_ = nullptr;        /**< 根容器，包含菜单栏和内容区 */
    lv_obj_t* menu_panel_ = nullptr;   /**< 左侧菜单栏容器 */
    lv_obj_t* content_panel_ = nullptr;/**< 右侧内容区容器 */
    std::vector<PageEntry> entries_;   /**< 已注册页面列表 */
    Page* current_ = nullptr;          /**< 当前显示的页面 */

    /** 根据名称查找对应的页面条目 */
    PageEntry* find_entry(const std::string& name);

    /** 菜单按钮点击事件的静态回调函数 */
    static void menu_btn_event_cb(lv_event_t* e);

    /** 根据当前活动页面名称，高亮对应的菜单按钮 */
    void update_menu_highlight(const std::string& active_name);
};

} // namespace ui

#endif
