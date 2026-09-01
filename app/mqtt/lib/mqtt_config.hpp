#ifndef UPC_MQTT_CONFIG_HPP
#define UPC_MQTT_CONFIG_HPP

#include <string>

namespace upc::mqtt {

/** MQTT 服务总开关：1=正常启动并运行；0=启动命令直接跳过服务逻辑 */
#ifndef UPC_MQTT_SERVICE_ENABLE
#define UPC_MQTT_SERVICE_ENABLE 1
#endif

/** MQTT 接收原始帧日志：1=打印 topic/payload，0=关闭；仍受 UPC_DEBUG/UPC_MOD_MQTT_SERVICE 总开关控制 */
#ifndef UPC_MQTT_RX_RAW_LOG_ENABLE
#define UPC_MQTT_RX_RAW_LOG_ENABLE 1
#endif

struct MqttConfig {
    /** 出厂烧录的设备 SN；MQTT clientId 与 username 均使用该值。 */
    std::string device_id;
    /** 出厂烧录的设备根密钥；仅用于激活 HMAC，绝不能记录到日志。 */
    std::string device_secret;
    /** 由激活服务 mqttEndpoint 下发并持久化，禁止在设备侧静态配置。 */
    std::string broker_uri;
    std::string client_id;
    std::string username;
    std::string password;
    /** 设备初始化服务 HTTP(S) 地址。 */
    std::string activation_base_url;
    /** 激活后 MQTT 凭据的本地持久化位置。生产环境应位于加密存储区。 */
    std::string credential_file;
    int keepalive_sec = 60;
    int qos_telemetry = 1;
    int qos_heartbeat = 1;
    int qos_ack = 1;
    int heartbeat_interval_sec = 30;
    bool clean_session = true;

    [[nodiscard]] std::string topic_prefix() const;
    [[nodiscard]] std::string topic_cmd() const;
    [[nodiscard]] std::string topic_cmd_ack() const;
    [[nodiscard]] std::string topic_telemetry_chamber() const;
    [[nodiscard]] std::string topic_heartbeat() const;
    [[nodiscard]] std::string topic_state() const;
    [[nodiscard]] std::string topic_ota_notify() const;
    [[nodiscard]] std::string topic_ota_manifest_req() const;
    [[nodiscard]] std::string topic_ota_manifest_resp() const;
    [[nodiscard]] std::string topic_ota_progress() const;
};

MqttConfig load_mqtt_config();

} // namespace upc::mqtt

#endif
