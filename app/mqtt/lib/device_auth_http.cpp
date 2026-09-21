#include "device_auth_http.hpp"

#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
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
        error = "设备认证服务地址必须以 http:// 或 https:// 开头";
        return false;
    }

    const size_t path_begin = url.find('/', scheme_len);
    const std::string authority = url.substr(
        scheme_len, path_begin == std::string::npos ? std::string::npos : path_begin - scheme_len);
    const size_t colon = authority.rfind(':');
    if(authority.empty() || colon == std::string::npos || colon == 0 ||
       colon + 1 >= authority.size()) {
        error = "设备认证服务地址必须包含 host:port";
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
        error = "无法解析设备认证服务主机";
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
        error = "无法连接设备认证服务";
    }
    return fd;
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

std::string timestamp_ms()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

bool device_auth_random_hex(size_t bytes, std::string& out, std::string& error)
{
    std::string buffer(bytes, '\0');
    if(RAND_bytes(reinterpret_cast<unsigned char*>(buffer.data()),
                  static_cast<int>(buffer.size())) != 1) {
        error = "无法生成认证随机数";
        return false;
    }
    out = hex_encode(reinterpret_cast<const unsigned char*>(buffer.data()), buffer.size());
    return true;
}

std::string device_auth_hmac_sha256_hex(const std::string& device_secret,
                                        const std::string& sign_data)
{
    unsigned char key[SHA256_DIGEST_LENGTH]{};
    SHA256(reinterpret_cast<const unsigned char*>(device_secret.data()),
           device_secret.size(), key);
    unsigned char signature[EVP_MAX_MD_SIZE]{};
    unsigned int signature_size = 0;
    if(!HMAC(EVP_sha256(), key, sizeof(key),
             reinterpret_cast<const unsigned char*>(sign_data.data()), sign_data.size(),
             signature, &signature_size) ||
       signature_size == 0) {
        return {};
    }
    return hex_encode(signature, signature_size);
}

std::string device_auth_json_escape(const std::string& value)
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

bool device_auth_json_extract_string(const std::string& json, const char* key, std::string& out)
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

bool device_auth_response_ok(int status, const std::string& response)
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

bool device_auth_post_json(const std::string& base_url,
                           const std::string& endpoint,
                           const std::string& body,
                           int& status,
                           std::string& response,
                           std::string& error)
{
    std::string nonce;
    if(!device_auth_random_hex(8, nonce, error)) {
        return false;
    }
    const std::string timestamp = timestamp_ms();

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
            error = "无法初始化设备认证服务 TLS 根证书";
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
            error = "设备认证服务 TLS 握手或证书验证失败";
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
            error = "认证请求发送失败";
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
            error = "认证响应读取失败";
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
        error = "设备认证服务返回了无效 HTTP 响应";
        return false;
    }
    status = std::atoi(raw_response.c_str() + first_space + 1);
    response = raw_response.substr(body_begin + 4);
    return true;
}

} // namespace upc::mqtt
