#ifndef UPC_GLAVO_N2_BLE_TRANSPORT_HPP
#define UPC_GLAVO_N2_BLE_TRANSPORT_HPP

/**
 * @file n2_ble_transport.hpp
 * @brief BlueZ D-Bus GATT Client 封装。无 libdbus 时为空实现。
 *
 * 本板 bluetoothd 的 Device1.Connect/Disconnect 带 ADDR_TYPE 字符串参数；
 * 传输层先按该签名调用，失败再回退上游空参。
 */

#include <string>

namespace upc::glavo {

class N2BleTransport {
public:
    struct Device {
        std::string path;
        std::string address;
        std::string address_type;
        std::string name;
    };

    N2BleTransport();
    ~N2BleTransport();

    N2BleTransport(const N2BleTransport&) = delete;
    N2BleTransport& operator=(const N2BleTransport&) = delete;

    static bool compiled_with_bluez();

    bool open();
    void close();
    bool start_discovery();
    void stop_discovery();
    /** 抽一次 D-Bus，timeout_ms 内阻塞。 */
    void poll(int timeout_ms);
    bool find_n2_machine(Device* out);
    bool connect_device(const Device& dev);
    bool disconnect_device(const Device& dev);
    bool device_connected(const std::string& path);
    bool services_resolved(const std::string& path);
    bool find_n2_characteristic(const std::string& device_path, std::string* char_path);
    bool start_notify(const std::string& char_path);
    bool stop_notify(const std::string& char_path);
    bool take_notify_ascii(std::string* ascii);
    bool read_ascii(const std::string& char_path, std::string* ascii);

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace upc::glavo

#endif /* UPC_GLAVO_N2_BLE_TRANSPORT_HPP */
