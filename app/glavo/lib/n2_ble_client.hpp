#ifndef UPC_GLAVO_N2_BLE_CLIENT_HPP
#define UPC_GLAVO_N2_BLE_CLIENT_HPP

/**
 * @file n2_ble_client.hpp
 * @brief 制氮机 BLE 客户端：独立线程扫描/连接/解析，业务侧拿快照或经回调发 IPC。
 */

#include "n2_ble_parse.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace upc::glavo {

struct N2BleSnapshot {
    bool        online    = false;
    bool        n2_valid  = false;
    float       n2_percent = 0.f;
    N2BleState  state     = N2BleState::Unknown;
    uint32_t    seq       = 0;
    std::string ascii;
};

class N2BleClient {
public:
    using StatusCb = std::function<void(const N2BleSnapshot&)>;

    static N2BleClient& instance();

    /** 启动后台线程。无 BlueZ 时只 Warn，不拖垮 Glavo。 */
    bool init(StatusCb cb);
    void shutdown();
    N2BleSnapshot snapshot() const;

    N2BleClient(const N2BleClient&) = delete;
    N2BleClient& operator=(const N2BleClient&) = delete;

private:
    N2BleClient() = default;
    ~N2BleClient();

    void run();
    void publish_if_needed(const N2BleSnapshot& snap, bool force);

    StatusCb cb_;
    mutable std::mutex mu_;
    N2BleSnapshot snap_{};
    std::thread th_;
    std::atomic<bool> running_{false};
    std::chrono::steady_clock::time_point last_pub_{};
};

} // namespace upc::glavo

#endif /* UPC_GLAVO_N2_BLE_CLIENT_HPP */
