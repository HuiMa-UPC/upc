#ifndef UPC_DEVICE_ACTIVATION_HPP
#define UPC_DEVICE_ACTIVATION_HPP

#include "mqtt_config.hpp"

#include <string>

namespace upc::mqtt {

/**
 * 激活服务返回并需由设备安全存储持久化的 MQTT 凭据。
 */
struct ActivatedMqttCredential {
    std::string mqtt_password;
    std::string mqtt_endpoint;
    /** 关窗前保留的上一轮 mqttPassword；空表示无需回退。 */
    std::string mqtt_password_prev;
};

/**
 * 设备侧首次激活客户端。
 *
 * 按 SNOWX IoT Device Activation Security Spec v1.5 的 init/complete
 * 挑战-响应流程工作。由于云端保存的是 sha256(deviceSecret)，设备侧同样
 * 以该摘要的原始字节作为 HMAC key，保持与 mock_device_activate.py 一致。
 */
class DeviceActivationClient {
public:
    explicit DeviceActivationClient(const MqttConfig& config);

    /**
     * 完成一次激活。成功时返回 true 并填写 out；失败原因写入 error。
     * 此调用会进行网络 I/O，应从 MQTT 主循环以外的工作线程调用。
     */
    [[nodiscard]] bool activate(ActivatedMqttCredential& out, std::string& error) const;

private:
    MqttConfig config_;
};

/** 加载、原子保存和清除激活后获得的 MQTT 凭据。 */
class MqttCredentialStore {
public:
    explicit MqttCredentialStore(std::string file_path);

    [[nodiscard]] bool load(ActivatedMqttCredential& out, std::string& error) const;
    [[nodiscard]] bool save(const ActivatedMqttCredential& credential,
                            std::string& error) const;
    [[nodiscard]] bool clear(std::string& error) const;

private:
    std::string file_path_;
};

/** 将服务端 host:port 或 mqtts:// URI 转为 Paho C 使用的 server URI。 */
[[nodiscard]] std::string normalise_broker_uri(const std::string& endpoint);

} // namespace upc::mqtt

#endif
