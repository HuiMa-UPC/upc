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
    std::string device_id = "SNOWPOD-TEST-001";
    std::string broker_uri = "tcp://172.17.0.72:1883";
    std::string client_id;
    std::string username;
    std::string password;
    int keepalive_sec = 60;
    int qos_telemetry = 1;
    int qos_ack = 1;
    bool clean_session = true;

    [[nodiscard]] std::string topic_prefix() const;
    [[nodiscard]] std::string topic_cmd() const;
    [[nodiscard]] std::string topic_cmd_ack() const;
    [[nodiscard]] std::string topic_telemetry_chamber() const;
};

MqttConfig load_mqtt_config();

} // namespace upc::mqtt

#endif
