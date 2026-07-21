#ifndef UPC_GLAVO_WIFI_MANAGER_HPP
#define UPC_GLAVO_WIFI_MANAGER_HPP

/**
 * @file wifi_manager.hpp
 * @brief WiFi 管理：直接驱动 wpa_supplicant 控制接口（wpa_ctrl / libwpa_client）。
 *
 * 目标板由自定义脚本启动 wpa_supplicant（wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf，
 * 其中 ctrl_interface=/var/run/wpa_supplicant、update_config=1）。本类连其控制 socket
 * 收发命令：扫描 / 连接 / 忘记 / 已保存列表 / 状态；DHCP 默认交给系统（如 dhcpcd），
 * 系统未接管时回退 udhcpc。已保存网络由
 * wpa_supplicant 在开启时自动重连（依赖 SAVE_CONFIG 持久化 + update_config=1）。
 * 可用环境变量 UPC_WIFI_IFACE（默认 wlan0）、UPC_WPA_CTRL_DIR（默认 /var/run/wpa_supplicant）覆盖。
 *
 * 线程模型：一个后台线程，命令队列 + 周期轮询（默认 3s）。state/list 回调在该线程
 * 触发，内部只做 publish（MsgPort::publish_async 线程安全）。
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace upc::glavo {

/** WiFi 安全类型（数值对齐 middleware::WifiSecurity：Open=0 WPA2=1 WPA3=2）。 */
enum class WifiSecurity : uint32_t { Open = 0, Wpa2 = 1, Wpa3 = 2 };

/** 当前网络状态快照（首页图标 + 设置详情共用）。 */
struct WifiSnapshot {
    bool        initialized = false;  /**< wpa_supplicant 控制接口是否就绪 */
    bool        enabled     = false;  /**< WiFi 开关是否开 */
    bool        connected   = false;  /**< wpa_state==COMPLETED */
    int         signal_bars = 0;      /**< 0..4 */
    std::string ssid;                 /**< 当前连接 SSID，未连为空 */
    std::string ip;                   /**< 当前 IPv4，未连为空 */
};

/** 单个网络条目（扫描 / 已保存共用）。 */
struct WifiEntry {
    std::string  ssid;
    int          signal_bars = 0;                 /**< 0..4 */
    WifiSecurity security     = WifiSecurity::Open;
    bool         saved        = false;
    bool         connected    = false;
};

/** 列表回调类型：扫描结果 or 已保存列表。 */
enum class WifiListKind { Scan, Saved };

class WifiManager {
public:
    using StateCb  = std::function<void(const WifiSnapshot&)>;
    using ListCb   = std::function<void(WifiListKind, const std::vector<WifiEntry>&)>;
    /** 连接结果回调：ok=false 表示失败（密码错/超时）。 */
    using ResultCb = std::function<void(bool ok, const std::string& ssid)>;

    static WifiManager& instance();

    /**
     * @brief 启动后台线程（连 wpa_supplicant 控制 socket + 周期轮询）。
     * @note 幂等：重复调用只更新回调。始终返回 true（socket 未就绪时线程内自动重连）。
     */
    bool init(StateCb state_cb, ListCb list_cb, ResultCb result_cb = nullptr);

    /** @brief 停止后台线程并关闭控制连接。 */
    void shutdown();

    /* ---- 命令（异步：投递到主循环线程执行，立即返回） ---- */
    void set_enabled(bool enable);
    void scan();
    void connect(const std::string& ssid, const std::string& password);
    void disconnect(const std::string& ssid);
    void forget(const std::string& ssid);
    void request_info();    /**< 主动推一次当前状态（GLAVO_Wifi_State_Ntf） */
    void request_saved();   /**< 推一次已保存列表（GLAVO_Wifi_Saved_Rsp） */

    /** @brief 线程安全读取当前状态快照（供首页 mcu_link 组包使用）。 */
    WifiSnapshot snapshot() const;

    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;

private:
    WifiManager() = default;
    ~WifiManager();

    struct Impl;      /**< pImpl：隐藏 wpa_ctrl / 线程等实现细节 */
    Impl* impl_ = nullptr;
};

} // namespace upc::glavo

#endif /* UPC_GLAVO_WIFI_MANAGER_HPP */
