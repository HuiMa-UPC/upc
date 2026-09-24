#include "credential_rotation.hpp"
#include "device_auth_http.hpp"

namespace upc::mqtt {

DeviceCredentialRotationClient::DeviceCredentialRotationClient(const MqttConfig& config)
    : config_(config)
{
}

bool DeviceCredentialRotationClient::rotate(std::string& out_password, std::string& error) const
{
    out_password.clear();
    if(config_.device_id.empty() || config_.device_secret.empty()) {
        error = "缺少出厂 deviceSn 或 deviceSecret";
        return false;
    }

    std::string client_nonce;
    if(!device_auth_random_hex(8, client_nonce, error)) {
        return false;
    }
    const std::string init_body =
        "{\"deviceSn\":\"" + device_auth_json_escape(config_.device_id) +
        "\",\"clientNonce\":\"" + client_nonce + "\"}";

    int status = 0;
    std::string init_response;
    if(!device_auth_post_json(config_.activation_base_url, "/api/v1/rotate-credential/init",
                              init_body, status, init_response, error)) {
        return false;
    }
    if(!device_auth_response_ok(status, init_response)) {
        error = "凭证轮换 init 被服务端拒绝，HTTP " + std::to_string(status);
        return false;
    }

    std::string server_nonce;
    if(!device_auth_json_extract_string(init_response, "serverNonce", server_nonce) ||
       server_nonce.empty()) {
        error = "凭证轮换 init 响应缺少 serverNonce";
        return false;
    }

    const std::string sign_data = server_nonce + client_nonce + config_.device_id;
    const std::string signature = device_auth_hmac_sha256_hex(config_.device_secret, sign_data);
    if(signature.empty()) {
        error = "无法计算凭证轮换 HMAC";
        return false;
    }

    const std::string rotate_body =
        "{\"deviceSn\":\"" + device_auth_json_escape(config_.device_id) +
        "\",\"clientNonce\":\"" + client_nonce +
        "\",\"serverNonce\":\"" + device_auth_json_escape(server_nonce) +
        "\",\"signature\":\"" + signature + "\"}";

    std::string rotate_response;
    if(!device_auth_post_json(config_.activation_base_url, "/api/v1/rotate-credential",
                              rotate_body, status, rotate_response, error)) {
        return false;
    }
    if(!device_auth_response_ok(status, rotate_response)) {
        error = "凭证轮换被服务端拒绝，HTTP " + std::to_string(status);
        return false;
    }
    if(!device_auth_json_extract_string(rotate_response, "mqttPassword", out_password) ||
       out_password.empty()) {
        error = "凭证轮换响应缺少 mqttPassword";
        return false;
    }
    return true;
}

} // namespace upc::mqtt
