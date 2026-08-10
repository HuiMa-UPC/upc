#include "device_activation.hpp"

#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <netdb.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace upc::mqtt {

namespace {

constexpr int kHttpTimeoutSeconds = 10;

struct ParsedUrl {
    bool tls = false;
    std::string host;
    std::string port;
    std::string path;
};

bool parse_url(const std::string& url, ParsedUrl& out, std::string& error)
{
    constexpr const char* kHttp = "http://";
    constexpr const char* kHttps = "https://";
    size_t scheme_len = 0;
    if(url.rfind(kHttp, 0) == 0) {
        out.tls = false;
        scheme_len = std::strlen(kHttp);
    } else if(url.rfind(kHttps, 0) == 0) {
        out.tls = true;
        scheme_len = std::strlen(kHttps);
    } else {
        error = "激活服务地址必须以 http:// 或 https:// 开头";
        return false;
    }

    const size_t path_begin = url.find('/', scheme_len);
    const std::string authority = url.substr(
        scheme_len, path_begin == std::string::npos ? std::string::npos : path_begin - scheme_len);
    const size_t colon = authority.rfind(':');
    if(authority.empty() || colon == std::string::npos || colon == 0 ||
       colon + 1 >= authority.size()) {
        error = "激活服务地址必须包含 host:port";
        return false;
    }

    out.host = authority.substr(0, colon);
    out.port = authority.substr(colon + 1);
    out.path = path_begin == std::string::npos ? "" : url.substr(path_begin);
    return true;
}

int connect_socket(const ParsedUrl& url, std::string& error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    if(getaddrinfo(url.host.c_str(), url.port.c_str(), &hints, &results) != 0) {
        error = "无法解析激活服务主机";
        return -1;
    }

    int fd = -1;
    for(addrinfo* item = results; item; item = item->ai_next) {
        fd = ::socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if(fd < 0) {
            continue;
        }
        timeval timeout{};
        timeout.tv_sec = kHttpTimeoutSeconds;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if(::connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(results);

    if(fd < 0) {
        error = "无法连接激活服务";
    }
    return fd;
}

bool json_extract_string(const std::string& json, const char* key, std::string& out)
{
    const std::string token = "\"" + std::string(key) + "\"";
    size_t pos = json.find(token);
    if(pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + token.size());
    if(pos == std::string::npos) {
        return false;
    }
    pos = json.find('"', pos + 1);
    if(pos == std::string::npos) {
        return false;
    }
    const size_t begin = ++pos;
    bool escaped = false;
    while(pos < json.size()) {
        if(json[pos] == '"' && !escaped) {
            out.assign(json, begin, pos - begin);
            return true;
        }
        escaped = json[pos] == '\\' && !escaped;
        if(json[pos] != '\\') {
            escaped = false;
        }
        ++pos;
    }
    return false;
}

std::string json_escape(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for(const char c : value) {
        if(c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string hex_encode(const unsigned char* bytes, size_t size)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(size * 2, '\0');
    for(size_t i = 0; i < size; ++i) {
        out[i * 2] = kHex[(bytes[i] >> 4U) & 0x0fU];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0fU];
    }
    return out;
}

bool random_hex(size_t bytes, std::string& out, std::string& error)
{
    std::string buffer(bytes, '\0');
    if(RAND_bytes(reinterpret_cast<unsigned char*>(buffer.data()),
                  static_cast<int>(buffer.size())) != 1) {
        error = "无法生成激活随机数";
        return false;
    }
    out = hex_encode(reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    return true;
}

bool post_json(const std::string& base_url, const std::string& endpoint,
               const std::string& body, const std::string& timestamp,
               const std::string& nonce, int& status, std::string& response,
               std::string& error)
{
    ParsedUrl url;
    if(!parse_url(base_url, url, error)) {
        return false;
    }
    if(!url.path.empty() && url.path.back() == '/') {
        url.path.pop_back();
    }
    url.path += endpoint;

    const int fd = connect_socket(url, error);
    if(fd < 0) {
        return false;
    }

    SSL_CTX* ssl_ctx = nullptr;
    SSL* ssl = nullptr;
    if(url.tls) {
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if(!ssl_ctx || SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
            error = "无法初始化激活服务 TLS 根证书";
            ::close(fd);
            SSL_CTX_free(ssl_ctx);
            return false;
        }
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, nullptr);
        ssl = SSL_new(ssl_ctx);
        if(!ssl || SSL_set_tlsext_host_name(ssl, url.host.c_str()) != 1 ||
           SSL_set1_host(ssl, url.host.c_str()) != 1 ||
           SSL_set_fd(ssl, fd) != 1 || SSL_connect(ssl) != 1 ||
           SSL_get_verify_result(ssl) != X509_V_OK) {
            error = "激活服务 TLS 握手或证书验证失败";
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
            ::close(fd);
            return false;
        }
    }

    const std::string request =
        "POST " + url.path + " HTTP/1.1\r\n"
        "Host: " + url.host + "\r\n"
        "Content-Type: application/json\r\n"
        "X-Timestamp: " + timestamp + "\r\n"
        "X-Nonce: " + nonce + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;

    size_t sent = 0;
    while(sent < request.size()) {
        const int written = ssl
            ? SSL_write(ssl, request.data() + sent, static_cast<int>(request.size() - sent))
            : static_cast<int>(::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL));
        if(written <= 0) {
            error = "激活请求发送失败";
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
            ::close(fd);
            return false;
        }
        sent += static_cast<size_t>(written);
    }

    std::string raw_response;
    char buffer[1024];
    for(;;) {
        const int read_size = ssl
            ? SSL_read(ssl, buffer, sizeof(buffer))
            : static_cast<int>(::recv(fd, buffer, sizeof(buffer), 0));
        if(read_size == 0) {
            break;
        }
        if(read_size < 0) {
            error = "激活响应读取失败";
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
            ::close(fd);
            return false;
        }
        raw_response.append(buffer, static_cast<size_t>(read_size));
    }
    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);
    ::close(fd);

    const size_t first_space = raw_response.find(' ');
    const size_t line_end = raw_response.find("\r\n");
    const size_t body_begin = raw_response.find("\r\n\r\n");
    if(first_space == std::string::npos || line_end == std::string::npos ||
       body_begin == std::string::npos) {
        error = "激活服务返回了无效 HTTP 响应";
        return false;
    }
    status = std::atoi(raw_response.c_str() + first_space + 1);
    response = raw_response.substr(body_begin + 4);
    return true;
}

bool response_is_success(int status, const std::string& response)
{
    if(status < 200 || status >= 300) {
        return false;
    }
    const size_t code_pos = response.find("\"code\"");
    if(code_pos == std::string::npos) {
        return true;
    }
    const size_t colon = response.find(':', code_pos);
    return colon != std::string::npos && std::atoi(response.c_str() + colon + 1) == 200;
}

} // namespace

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
    std::string init_nonce;
    if(!random_hex(8, client_nonce, error) || !random_hex(8, init_nonce, error)) {
        return false;
    }
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const std::string init_timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    const std::string init_body =
        "{\"deviceSn\":\"" + json_escape(config_.device_id) +
        "\",\"clientNonce\":\"" + client_nonce + "\"}";

    int status = 0;
    std::string init_response;
    if(!post_json(config_.activation_base_url, "/internal/device/activate/init",
                  init_body, init_timestamp, init_nonce, status, init_response, error)) {
        return false;
    }
    if(!response_is_success(status, init_response)) {
        error = "激活 init 被服务端拒绝，HTTP " + std::to_string(status);
        return false;
    }

    std::string server_nonce;
    if(!json_extract_string(init_response, "serverNonce", server_nonce) ||
       server_nonce.empty()) {
        error = "激活 init 响应缺少 serverNonce";
        return false;
    }

    unsigned char key[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(config_.device_secret.data()),
           config_.device_secret.size(), key);
    const std::string sign_data = server_nonce + client_nonce + config_.device_id;
    unsigned char signature[EVP_MAX_MD_SIZE]{};
    unsigned int signature_size = 0;
    if(!HMAC(EVP_sha256(), key, sizeof(key),
             reinterpret_cast<const unsigned char*>(sign_data.data()), sign_data.size(),
             signature, &signature_size)) {
        error = "无法计算激活 HMAC";
        return false;
    }

    std::string complete_nonce;
    if(!random_hex(8, complete_nonce, error)) {
        return false;
    }
    const std::string complete_timestamp = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const std::string complete_body =
        "{\"deviceSn\":\"" + json_escape(config_.device_id) +
        "\",\"clientNonce\":\"" + client_nonce +
        "\",\"serverNonce\":\"" + json_escape(server_nonce) +
        "\",\"signature\":\"" + hex_encode(signature, signature_size) + "\"}";

    std::string complete_response;
    if(!post_json(config_.activation_base_url, "/internal/device/activate",
                  complete_body, complete_timestamp, complete_nonce,
                  status, complete_response, error)) {
        return false;
    }
    if(!response_is_success(status, complete_response)) {
        error = "激活被服务端拒绝，HTTP " + std::to_string(status);
        return false;
    }
    if(!json_extract_string(complete_response, "mqttPassword", out.mqtt_password) ||
       out.mqtt_password.empty()) {
        error = "激活响应缺少 mqttPassword";
        return false;
    }
    if(!json_extract_string(complete_response, "mqttEndpoint", out.mqtt_endpoint) ||
       out.mqtt_endpoint.empty()) {
        error = "激活响应缺少 mqttEndpoint";
        return false;
    }
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
       credential.mqtt_endpoint.find_first_of("\r\n") != std::string::npos) {
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
    const std::string content = "mqttPassword=" + credential.mqtt_password +
                                "\nmqttEndpoint=" + credential.mqtt_endpoint + "\n";
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
