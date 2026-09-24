#ifndef UPC_CREDENTIAL_ROTATION_HPP
#define UPC_CREDENTIAL_ROTATION_HPP

#include "mqtt_config.hpp"

#include <string>

namespace upc::mqtt {

/**
 * 云端触发的 mqttPassword 轮换客户端。
 *
 * 按《设备端凭证轮换对接手册》v1.1：init → HMAC → rotate，本轮只打这一次，
 * 失败由调用方回退旧密码、等下一周期，不在客户端内重试（单 SN 每小时 3 次限额）。
 */
class DeviceCredentialRotationClient {
public:
    explicit DeviceCredentialRotationClient(const MqttConfig& config);

    /**
     * 完成一次轮换 HTTP。成功时 out_password 为新 mqttPassword。
     * 此调用会进行网络 I/O，应从 MQTT 主循环以外的工作线程调用。
     */
    [[nodiscard]] bool rotate(std::string& out_password, std::string& error) const;

private:
    MqttConfig config_;
};

} // namespace upc::mqtt

#endif
