#include "device_activation.hpp"
#include "device_auth_http.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace upc::mqtt {

DeviceActivationClient::DeviceActivationClient(const MqttConfig& config)
    : config_(config)
{
}

bool DeviceActivationClient::activate(ActivatedMqttCredential& out, std::string& error) const
{
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
    if(!device_auth_post_json(config_.activation_base_url, "/api/v1/devices/activate/init",
                              init_body, status, init_response, error)) {
        return false;
    }
    if(!device_auth_response_ok(status, init_response)) {
        error = "激活 init 被服务端拒绝，HTTP " + std::to_string(status);
        return false;
    }

    std::string server_nonce;
    if(!device_auth_json_extract_string(init_response, "serverNonce", server_nonce) ||
       server_nonce.empty()) {
        error = "激活 init 响应缺少 serverNonce";
        return false;
    }

    const std::string sign_data = server_nonce + client_nonce + config_.device_id;
    const std::string signature = device_auth_hmac_sha256_hex(config_.device_secret, sign_data);
    if(signature.empty()) {
        error = "无法计算激活 HMAC";
        return false;
    }

    const std::string complete_body =
        "{\"deviceSn\":\"" + device_auth_json_escape(config_.device_id) +
        "\",\"clientNonce\":\"" + client_nonce +
        "\",\"serverNonce\":\"" + device_auth_json_escape(server_nonce) +
        "\",\"signature\":\"" + signature + "\"}";

    std::string complete_response;
    if(!device_auth_post_json(config_.activation_base_url, "/api/v1/devices/activate",
                              complete_body, status, complete_response, error)) {
        return false;
    }
    if(!device_auth_response_ok(status, complete_response)) {
        error = "激活被服务端拒绝，HTTP " + std::to_string(status);
        return false;
    }
    if(!device_auth_json_extract_string(complete_response, "mqttPassword", out.mqtt_password) ||
       out.mqtt_password.empty()) {
        error = "激活响应缺少 mqttPassword";
        return false;
    }
    if(!device_auth_json_extract_string(complete_response, "mqttEndpoint", out.mqtt_endpoint) ||
       out.mqtt_endpoint.empty()) {
        error = "激活响应缺少 mqttEndpoint";
        return false;
    }
    out.mqtt_password_prev.clear();
    return true;
}

MqttCredentialStore::MqttCredentialStore(std::string file_path)
    : file_path_(std::move(file_path))
{
}

bool MqttCredentialStore::load(ActivatedMqttCredential& out, std::string& error) const
{
    std::ifstream input(file_path_);
    if(!input.is_open()) {
        if(errno == ENOENT) {
            return false;
        }
        error = "无法读取 MQTT 凭据文件";
        return false;
    }
    std::string line;
    while(std::getline(input, line)) {
        const size_t separator = line.find('=');
        if(separator == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if(key == "mqttPassword") {
            out.mqtt_password = value;
        } else if(key == "mqttEndpoint") {
            out.mqtt_endpoint = value;
        } else if(key == "mqttPasswordPrev") {
            out.mqtt_password_prev = value;
        }
    }
    if(out.mqtt_password.empty() || out.mqtt_endpoint.empty()) {
        error = "MQTT 凭据文件缺少 mqttPassword 或 mqttEndpoint";
        return false;
    }
    return true;
}

bool MqttCredentialStore::save(const ActivatedMqttCredential& credential,
                               std::string& error) const
{
    if(credential.mqtt_password.empty() || credential.mqtt_endpoint.empty() ||
       credential.mqtt_password.find_first_of("\r\n") != std::string::npos ||
       credential.mqtt_endpoint.find_first_of("\r\n") != std::string::npos ||
       credential.mqtt_password_prev.find_first_of("\r\n") != std::string::npos) {
        error = "拒绝保存无效 MQTT 凭据";
        return false;
    }

    const std::filesystem::path path(file_path_);
    std::error_code ec;
    if(path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if(ec) {
            error = "无法创建 MQTT 凭据目录";
            return false;
        }
    }

    const std::string temporary = file_path_ + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if(fd < 0) {
        error = "无法创建 MQTT 凭据文件";
        return false;
    }
    std::string content = "mqttPassword=" + credential.mqtt_password +
                          "\nmqttEndpoint=" + credential.mqtt_endpoint + "\n";
    if(!credential.mqtt_password_prev.empty()) {
        content += "mqttPasswordPrev=" + credential.mqtt_password_prev + "\n";
    }
    const ssize_t written = ::write(fd, content.data(), content.size());
    const bool write_ok = written == static_cast<ssize_t>(content.size()) &&
                          ::fsync(fd) == 0 && ::close(fd) == 0;
    if(!write_ok) {
        (void)::close(fd);
        (void)::unlink(temporary.c_str());
        error = "无法写入 MQTT 凭据文件";
        return false;
    }
    if(::rename(temporary.c_str(), file_path_.c_str()) != 0) {
        (void)::unlink(temporary.c_str());
        error = "无法原子替换 MQTT 凭据文件";
        return false;
    }
    return true;
}

bool MqttCredentialStore::clear(std::string& error) const
{
    if(::unlink(file_path_.c_str()) == 0 || errno == ENOENT) {
        return true;
    }
    error = "无法清除 MQTT 凭据文件";
    return false;
}

std::string normalise_broker_uri(const std::string& endpoint)
{
    if(endpoint.empty()) {
        return {};
    }
    if(endpoint.rfind("mqtts://", 0) == 0) {
        return "ssl://" + endpoint.substr(std::strlen("mqtts://"));
    }
    if(endpoint.rfind("mqtt://", 0) == 0) {
        return "tcp://" + endpoint.substr(std::strlen("mqtt://"));
    }
    if(endpoint.find("://") == std::string::npos) {
        return "tcp://" + endpoint;
    }
    return endpoint;
}

} // namespace upc::mqtt
