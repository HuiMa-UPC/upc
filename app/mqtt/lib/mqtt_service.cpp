#include "mqtt_service.hpp"

#include "mqtt_service_internal.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>

namespace upc::mqtt {

#ifndef UPC_SW_VERSION
#define UPC_SW_VERSION "unknown"
#endif

#if UPC_DEBUG && UPC_MOD_MQTT_SERVICE
/**
 * @brief 打印 MQTT 收发原始帧。
 *
 * JSON 可打印字符保持原样，控制字符/二进制字节转义为 \xNN；分块限制单条日志长度，
 * 避免超过 rs_log 的消息缓冲区。该函数完全受 UPC_MOD_MQTT_SERVICE 控制。
 *
 * 每行 payload 都带 topic：分块后各行会散落在同一日志文件里，缺少 topic 就无法分辨
 * 属于哪条消息；同时 rs_log 依赖 "payload topic=" 把原始帧归入 topic_mqtt_wire.log。
 */
void debug_log_mqtt_frame(const char* direction,
                          const std::string& topic,
                          const void* payload,
                          size_t payload_len,
                          int qos,
                          int retained)
{
    const char* dir = direction ? direction : "?";
    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT %s frame topic=%s qos=%d retained=%d len=%zu",
        dir, topic.c_str(), qos, retained, payload_len);

    if(!payload || payload_len == 0) {
        UPC_LOG_MQTT_SERVICE(Debug, "MQTT %s payload topic=%s <empty>",
                             dir, topic.c_str());
        return;
    }

    // 单条日志过长可能被后端日志链路截断；保守缩短每块长度，确保每块都能完整落盘。
    constexpr size_t kMaxEncodedChunk = 512;
    const auto* bytes = static_cast<const unsigned char*>(payload);
    size_t offset = 0;
    size_t chunk_idx = 0;
    while(offset < payload_len) {
        ++chunk_idx;
        const size_t begin = offset;
        std::string encoded;
        encoded.reserve(kMaxEncodedChunk);

        while(offset < payload_len) {
            const unsigned char c = bytes[offset];
            char escaped[5]{};
            const char* token = escaped;
            size_t token_len = 0;

            switch(c) {
            case '\\': token = "\\\\"; token_len = 2; break;
            case '"':  token = "\\\""; token_len = 2; break;
            case '\n': token = "\\n";  token_len = 2; break;
            case '\r': token = "\\r";  token_len = 2; break;
            case '\t': token = "\\t";  token_len = 2; break;
            default:
                if(c >= 0x20 && c <= 0x7e) {
                    escaped[0] = static_cast<char>(c);
                    token_len = 1;
                } else {
                    std::snprintf(escaped, sizeof(escaped), "\\x%02X",
                                  static_cast<unsigned>(c));
                    token_len = 4;
                }
                break;
            }

            if(!encoded.empty() && encoded.size() + token_len > kMaxEncodedChunk) {
                break;
            }
            encoded.append(token, token_len);
            ++offset;
        }

        UPC_LOG_MQTT_SERVICE(Debug,
            "MQTT %s payload topic=%s chunk=%zu range[%zu..%zu/%zu]=\"%s\"",
            dir, topic.c_str(), chunk_idx, begin, offset, payload_len,
            encoded.c_str());
    }
}
#else
void debug_log_mqtt_frame(const char*, const std::string&, const void*,
                          size_t, int, int)
{
}
#endif

bool parse_url(const std::string& raw, ParsedUrl& out)
{
    out = ParsedUrl{};
    const size_t p = raw.find("://");
    if(p == std::string::npos) {
        return false;
    }
    out.scheme = raw.substr(0, p);
    std::transform(out.scheme.begin(), out.scheme.end(), out.scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if(out.scheme != "http" && out.scheme != "https") {
        return false;
    }
    const size_t host_begin = p + 3;
    size_t path_pos = raw.find('/', host_begin);
    std::string host_port =
        (path_pos == std::string::npos) ? raw.substr(host_begin)
                                        : raw.substr(host_begin, path_pos - host_begin);
    out.path = (path_pos == std::string::npos) ? "/" : raw.substr(path_pos);
    if(host_port.empty()) {
        return false;
    }

    size_t colon = host_port.rfind(':');
    if(colon != std::string::npos && host_port.find(']') == std::string::npos) {
        out.host = host_port.substr(0, colon);
        const int port = std::atoi(host_port.substr(colon + 1).c_str());
        if(port <= 0 || port > 65535) {
            return false;
        }
        out.port = static_cast<uint16_t>(port);
    } else {
        out.host = host_port;
        out.port = (out.scheme == "https") ? 443 : 80;
    }
    return !out.host.empty();
}

int connect_tcp_socket(const std::string& host, uint16_t port)
{
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    if(::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        return -1;
    }
    std::unique_ptr<struct addrinfo, decltype(&::freeaddrinfo)> guard(result, ::freeaddrinfo);
    for(struct addrinfo* ai = result; ai; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if(fd < 0) {
            continue;
        }
        if(::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            return fd;
        }
        ::close(fd);
    }
    return -1;
}

bool is_default_port_for_scheme(const std::string& scheme, uint16_t port)
{
    return (scheme == "http" && port == 80) ||
           (scheme == "https" && port == 443);
}

std::string make_host_header_value(const ParsedUrl& parsed)
{
    if(is_default_port_for_scheme(parsed.scheme, parsed.port)) {
        return parsed.host;
    }
    return parsed.host + ":" + std::to_string(parsed.port);
}

std::string trim_ascii_space(std::string s)
{
    size_t begin = 0;
    while(begin < s.size() &&
          (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) {
        ++begin;
    }
    size_t end = s.size();
    while(end > begin &&
          (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::string resolve_redirect_url(const ParsedUrl& base, const std::string& location_raw)
{
    const std::string location = trim_ascii_space(location_raw);
    if(location.empty()) {
        return {};
    }
    if(location.find("://") != std::string::npos) {
        return location;
    }

    const std::string authority = make_host_header_value(base);
    if(location[0] == '/') {
        return base.scheme + "://" + authority + location;
    }
    if(location[0] == '?') {
        std::string base_path = base.path;
        const size_t q = base_path.find('?');
        if(q != std::string::npos) {
            base_path = base_path.substr(0, q);
        }
        if(base_path.empty()) {
            base_path = "/";
        }
        return base.scheme + "://" + authority + base_path + location;
    }

    std::string base_path = base.path;
    const size_t q = base_path.find('?');
    if(q != std::string::npos) {
        base_path = base_path.substr(0, q);
    }
    const size_t slash = base_path.rfind('/');
    std::string dir = (slash == std::string::npos) ? "/" : base_path.substr(0, slash + 1);
    if(dir.empty()) {
        dir = "/";
    }
    return base.scheme + "://" + authority + dir + location;
}

std::string basename_from_url_path(const std::string& path)
{
    if(path.empty()) {
        return "firmware.bin";
    }
    const size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t q = name.find('?');
    if(q != std::string::npos) {
        name = name.substr(0, q);
    }
    if(name.empty()) {
        return "firmware.bin";
    }
    return name;
}

bool ensure_dir(const std::filesystem::path& p)
{
    std::error_code ec;
    if(std::filesystem::exists(p, ec)) {
        return std::filesystem::is_directory(p, ec);
    }
    return std::filesystem::create_directories(p, ec);
}

std::string hex_sha256_file(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if(!in) {
        return {};
    }
    std::array<char, 8192> buf{};

#if (defined(OPENSSL_VERSION_MAJOR) && OPENSSL_VERSION_MAJOR >= 3) || \
    OPENSSL_VERSION_NUMBER >= 0x30000000L
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digest_len = 0;
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if(!mdctx) {
        return {};
    }
    if(EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    while(in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if(n > 0 && EVP_DigestUpdate(mdctx, buf.data(), static_cast<size_t>(n)) != 1) {
            EVP_MD_CTX_free(mdctx);
            return {};
        }
    }
    if(EVP_DigestFinal_ex(mdctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(mdctx);
        return {};
    }
    EVP_MD_CTX_free(mdctx);
#else
    SHA256_CTX ctx{};
    SHA256_Init(&ctx);
    while(in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if(n > 0) {
            SHA256_Update(&ctx, buf.data(), static_cast<size_t>(n));
        }
    }
    unsigned char digest[SHA256_DIGEST_LENGTH]{};
    SHA256_Final(digest, &ctx);
    unsigned int digest_len = SHA256_DIGEST_LENGTH;
#endif

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(static_cast<size_t>(digest_len) * 2u);
    for(size_t i = 0; i < static_cast<size_t>(digest_len); ++i) {
        out[i * 2] = kHex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[digest[i] & 0xF];
    }
    return out;
}

bool download_file_with_progress(const std::string& url,
                                 const std::filesystem::path& out_file,
                                 const std::function<void(size_t, size_t)>& on_progress,
                                 std::string& err)
{
    err.clear();
    constexpr int kMaxRedirects = 3;
    std::string current_url = url;

    for(int redirect_count = 0; redirect_count <= kMaxRedirects; ++redirect_count) {
        ParsedUrl parsed{};
        if(!parse_url(current_url, parsed)) {
            err = "invalid_url";
            return false;
        }
        const int fd = connect_tcp_socket(parsed.host, parsed.port);
        if(fd < 0) {
            err = "connect_failed";
            return false;
        }

        SSL_CTX* ssl_ctx = nullptr;
        SSL* ssl = nullptr;
        if(parsed.scheme == "https") {
            ssl_ctx = SSL_CTX_new(TLS_client_method());
            if(!ssl_ctx) {
                ::close(fd);
                err = "ssl_ctx_failed";
                return false;
            }
            ssl = SSL_new(ssl_ctx);
            if(!ssl || SSL_set_fd(ssl, fd) != 1 || SSL_connect(ssl) != 1) {
                if(ssl) {
                    SSL_free(ssl);
                }
                SSL_CTX_free(ssl_ctx);
                ::close(fd);
                err = "ssl_connect_failed";
                return false;
            }
        }

        auto cleanup = [&]() {
            if(ssl) {
                SSL_shutdown(ssl);
                SSL_free(ssl);
                SSL_CTX_free(ssl_ctx);
            }
            ::close(fd);
        };

        auto write_all = [&](const char* data, size_t len) -> bool {
            size_t sent = 0;
            while(sent < len) {
                int n = 0;
                if(ssl) {
                    n = SSL_write(ssl, data + sent, static_cast<int>(len - sent));
                } else {
                    n = static_cast<int>(::send(fd, data + sent, len - sent, 0));
                }
                if(n <= 0) {
                    return false;
                }
                sent += static_cast<size_t>(n);
            }
            return true;
        };
        auto read_some = [&](char* data, size_t len) -> int {
            if(ssl) {
                return SSL_read(ssl, data, static_cast<int>(len));
            }
            return static_cast<int>(::recv(fd, data, len, 0));
        };

        const std::string request =
            "GET " + parsed.path + " HTTP/1.1\r\n"
            "Host: " + make_host_header_value(parsed) + "\r\n"
            "Connection: close\r\n"
            "User-Agent: snowpod-mqtt-ota/1.0\r\n\r\n";
        if(!write_all(request.data(), request.size())) {
            err = "http_send_failed";
            cleanup();
            return false;
        }

        std::string header;
        std::array<char, 4096> buf{};
        bool header_done = false;
        bool redirect_hit = false;
        bool out_opened = false;
        int status_code = 0;
        std::string redirect_location;
        size_t content_length = 0;
        size_t received = 0;
        std::ofstream out;

        for(;;) {
            const int n = read_some(buf.data(), buf.size());
            if(n <= 0) {
                break;
            }
            if(!header_done) {
                header.append(buf.data(), static_cast<size_t>(n));
                const size_t split = header.find("\r\n\r\n");
                if(split == std::string::npos) {
                    continue;
                }
                std::string header_text = header.substr(0, split + 4);
                std::string body = header.substr(split + 4);
                header_done = true;

                std::istringstream iss(header_text);
                std::string status_line;
                if(!std::getline(iss, status_line)) {
                    err = "bad_http_status";
                    break;
                }
                if(!status_line.empty() && status_line.back() == '\r') {
                    status_line.pop_back();
                }
                std::istringstream sl(status_line);
                std::string http_ver;
                if(!(sl >> http_ver >> status_code)) {
                    err = "bad_http_status";
                    break;
                }

                std::string line;
                while(std::getline(iss, line)) {
                    if(!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    const std::string lower = [&line]() {
                        std::string t = line;
                        std::transform(t.begin(), t.end(), t.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        return t;
                    }();
                    if(lower.rfind("content-length:", 0) == 0) {
                        content_length = static_cast<size_t>(std::strtoull(line.c_str() + 15, nullptr, 10));
                    } else if(lower.rfind("location:", 0) == 0) {
                        redirect_location = trim_ascii_space(line.substr(9));
                    }
                }

                if(status_code >= 300 && status_code < 400) {
                    if(!redirect_location.empty()) {
                        redirect_hit = true;
                        break;
                    }
                    err = "http_redirect_missing_location";
                    break;
                }
                if(status_code < 200 || status_code >= 300) {
                    err = "http_status_" + std::to_string(status_code);
                    break;
                }

                out.open(out_file, std::ios::binary | std::ios::trunc);
                if(!out) {
                    err = "open_tmp_failed";
                    break;
                }
                out_opened = true;
                if(!body.empty()) {
                    out.write(body.data(), static_cast<std::streamsize>(body.size()));
                    received += body.size();
                    if(on_progress) {
                        on_progress(received, content_length);
                    }
                }
                continue;
            }
            if(!out_opened) {
                continue;
            }
            out.write(buf.data(), n);
            received += static_cast<size_t>(n);
            if(on_progress) {
                on_progress(received, content_length);
            }
        }

        if(out_opened) {
            out.flush();
            out.close();
        }
        cleanup();

        if(redirect_hit) {
            if(redirect_count >= kMaxRedirects) {
                err = "http_redirect_too_many";
                return false;
            }
            const std::string next_url = resolve_redirect_url(parsed, redirect_location);
            if(next_url.empty()) {
                err = "http_redirect_bad_location";
                return false;
            }
            current_url = next_url;
            continue;
        }
        if(!header_done && err.empty()) {
            err = "http_recv_failed";
        }
        if(!err.empty()) {
            return false;
        }
        return true;
    }

    err = "http_redirect_too_many";
    return false;
}

bool upload_http_put(const std::string& url,
                     const void* body,
                     size_t body_len,
                     std::string& err,
                     const std::function<void(size_t, size_t)>& on_progress)
{
    err.clear();
    ParsedUrl parsed{};
    if(!parse_url(url, parsed)) {
        err = "invalid_url";
        return false;
    }
    const int fd = connect_tcp_socket(parsed.host, parsed.port);
    if(fd < 0) {
        err = "connect_failed";
        return false;
    }

    SSL_CTX* ssl_ctx = nullptr;
    SSL* ssl = nullptr;
    if(parsed.scheme == "https") {
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if(!ssl_ctx) {
            ::close(fd);
            err = "ssl_ctx_failed";
            return false;
        }
        ssl = SSL_new(ssl_ctx);
        if(!ssl || SSL_set_fd(ssl, fd) != 1 || SSL_connect(ssl) != 1) {
            if(ssl) {
                SSL_free(ssl);
            }
            SSL_CTX_free(ssl_ctx);
            ::close(fd);
            err = "ssl_connect_failed";
            return false;
        }
    }

    auto cleanup = [&]() {
        if(ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
        ::close(fd);
    };

    auto write_all = [&](const char* data, size_t len) -> bool {
        size_t sent = 0;
        while(sent < len) {
            int n = 0;
            if(ssl) {
                n = SSL_write(ssl, data + sent, static_cast<int>(len - sent));
            } else {
                n = static_cast<int>(::send(fd, data + sent, len - sent, 0));
            }
            if(n <= 0) {
                return false;
            }
            sent += static_cast<size_t>(n);
        }
        return true;
    };
    auto read_some = [&](char* data, size_t len) -> int {
        if(ssl) {
            return SSL_read(ssl, data, static_cast<int>(len));
        }
        return static_cast<int>(::recv(fd, data, len, 0));
    };

    /* Content-Type 必须与预签名一致，否则 Oss/S3 返回 403。 */
    const std::string request =
        "PUT " + parsed.path + " HTTP/1.1\r\n"
        "Host: " + make_host_header_value(parsed) + "\r\n"
        "Content-Type: " + std::string(kLogPutContentType) + "\r\n"
        "Content-Length: " + std::to_string(body_len) + "\r\n"
        "Connection: close\r\n"
        "User-Agent: snowpod-mqtt-log/1.0\r\n\r\n";
    if(!write_all(request.data(), request.size())) {
        err = "http_send_failed";
        cleanup();
        return false;
    }
    if(body_len > 0 && body) {
        /* 用户自定义进度回调：把 body 切成 64KB 块写出，每块完成后
           回调一次 "已发/总字节"，供日志采集进度上报使用。 */
        size_t body_sent = 0;
        const size_t total = body_len;
        const char* base = static_cast<const char*>(body);
        while(body_sent < total) {
            const size_t chunk = std::min<size_t>(64 * 1024, total - body_sent);
            if(!write_all(base + body_sent, chunk)) {
                err = "http_body_send_failed";
                cleanup();
                return false;
            }
            body_sent += chunk;
            if(on_progress) {
                on_progress(body_sent, total);
            }
        }
    }

    std::string header;
    std::array<char, 4096> buf{};
    int status_code = 0;
    bool header_done = false;
    for(;;) {
        const int n = read_some(buf.data(), buf.size());
        if(n <= 0) {
            break;
        }
        if(!header_done) {
            header.append(buf.data(), static_cast<size_t>(n));
            const size_t split = header.find("\r\n\r\n");
            if(split == std::string::npos) {
                continue;
            }
            header_done = true;
            std::istringstream iss(header.substr(0, split + 4));
            std::string status_line;
            if(!std::getline(iss, status_line)) {
                err = "bad_http_status";
                break;
            }
            if(!status_line.empty() && status_line.back() == '\r') {
                status_line.pop_back();
            }
            std::istringstream sl(status_line);
            std::string http_ver;
            if(!(sl >> http_ver >> status_code)) {
                err = "bad_http_status";
                break;
            }
            break;
        }
    }
    cleanup();

    if(!header_done) {
        if(err.empty()) {
            err = "http_recv_failed";
        }
        return false;
    }
    if(status_code < 200 || status_code >= 300) {
        err = "http_status_" + std::to_string(status_code);
        return false;
    }
    return true;
}

const char* component_target_dir(const std::string& component)
{
    if(component == "soc") {
        return kImageSocDir;
    }
    if(component == "mcu") {
        return kImageMcuDir;
    }
    if(component == "galvo") {
        return kImageGalvoDir;
    }
    return nullptr;
}

std::string sh_single_quote(const std::string& v)
{
    std::string out;
    out.reserve(v.size() + 8);
    out.push_back('\'');
    for(char c : v) {
        if(c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

std::string trim_line(std::string s)
{
    while(!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while(i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    return s.substr(i);
}

std::string strip_archive_leading_current_dir(std::string name)
{
    while(name.size() >= 2 && name[0] == '.' && name[1] == '/') {
        name.erase(0, 2);
    }
    return name;
}

bool archive_entry_safe_flat(const std::string& entry)
{
    const std::string normalized = strip_archive_leading_current_dir(entry);
    if(normalized.empty()) {
        return false;
    }
    if(normalized.find('/') != std::string::npos || normalized.find('\\') != std::string::npos) {
        return false;
    }
    if(normalized.find("..") != std::string::npos) {
        return false;
    }
    return true;
}

bool run_capture_lines(const std::string& cmd, std::vector<std::string>& lines)
{
    lines.clear();
    FILE* fp = ::popen(cmd.c_str(), "r");
    if(!fp) {
        return false;
    }
    char buf[1024];
    while(std::fgets(buf, sizeof(buf), fp)) {
        lines.emplace_back(trim_line(buf));
    }
    const int rc = ::pclose(fp);
    if(rc < 0) {
        return false;
    }
    if(WIFEXITED(rc)) {
        return WEXITSTATUS(rc) == 0;
    }
    return false;
}

bool run_shell_ok(const std::string& cmd)
{
    const int rc = std::system(cmd.c_str());
    if(rc < 0) {
        return false;
    }
    if(WIFEXITED(rc)) {
        return WEXITSTATUS(rc) == 0;
    }
    return false;
}

std::string to_upper_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool is_bin_name(const std::string& name)
{
    const std::string normalized = strip_archive_leading_current_dir(name);
    if(normalized.size() < 5) {
        return false;
    }
    const std::string upper = to_upper_ascii(normalized);
    return upper.rfind(".BIN") == upper.size() - 4;
}

bool is_img_name(const std::string& name)
{
    const std::string normalized = strip_archive_leading_current_dir(name);
    if(normalized.size() < 5) {
        return false;
    }
    const std::string upper = to_upper_ascii(normalized);
    return upper.rfind(".IMG") == upper.size() - 4;
}

bool fs_free_bytes(const char* path, uint64_t& out)
{
    out = 0;
    if(!path || !path[0]) {
        return false;
    }
    struct statvfs st{};
    if(::statvfs(path, &st) != 0) {
        return false;
    }
    /* 用 f_bavail 而非 f_bfree：后者含仅 root 可动用的保留块，普通进程写不到。 */
    out = static_cast<uint64_t>(st.f_bavail) * static_cast<uint64_t>(st.f_frsize);
    return true;
}

uint64_t activation_retry_interval_ms(uint32_t fail_count)
{
    if(fail_count <= 1) {
        return 5000;   // 第一次失败后：5s
    }
    if(fail_count == 2) {
        return 10000;  // 第二次失败后：10s
    }
    if(fail_count == 3) {
        return 30000;  // 第三次失败后：30s
    }
    return 60000;      // 第四次及以后：60s
}

MqttService::MqttService() = default;

MqttService::~MqttService()
{
    shutdown();
}

bool MqttService::init()
{
    cfg_ = load_mqtt_config();
    boot_id_ = read_boot_id();
    credential_store_ = MqttCredentialStore(cfg_.credential_file);

    ActivatedMqttCredential saved_credential;
    std::string credential_error;
    if(credential_store_.load(saved_credential, credential_error)) {
        cfg_.password = saved_credential.mqtt_password;
        cfg_.broker_uri = normalise_broker_uri(saved_credential.mqtt_endpoint);
        credentials_ready_ = true;
        UPC_LOG_MQTT_SERVICE(Info, "loaded locally activated MQTT credential");
    } else if(!credential_error.empty()) {
        UPC_LOG_MQTT_SERVICE(Warn, "cannot load local MQTT credential: %s",
                             credential_error.c_str());
    }

    subscribe_messages();

    UPC_LOG_MQTT_SERVICE(Info,
        "cfg broker=%s prefix=%s client=%s user=%s qos(tele=%d hb=%d ack=%d) hb=%ds credential=%s",
        cfg_.broker_uri.c_str(),
        cfg_.topic_prefix().c_str(),
        cfg_.client_id.c_str(),
        cfg_.username.c_str(),
        cfg_.qos_telemetry,
        cfg_.qos_heartbeat,
        cfg_.qos_ack,
        cfg_.heartbeat_interval_sec,
        credentials_ready_ ? "ready" : "activation-required");

    return true;
}

void MqttService::shutdown()
{
    join_activation_thread();
    {
        std::lock_guard<std::mutex> lock(ota_mtx_);
        ota_download_running_ = false;
    }
    if(ota_download_thread_.joinable()) {
        ota_download_thread_.join();
    }
    if(log_collect_thread_.joinable()) {
        log_collect_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(pending_mtx_);
        pending_acks_.clear();
    }
    subs_.clear();
    disconnect_mqtt();
    if(mqtt_created_) {
        MQTTClient_destroy(&client_);
        mqtt_created_ = false;
        client_ = nullptr;
    }
}

void MqttService::tick()
{
    if(!credentials_ready_) {
        poll_activation();
        return;
    }
    if(!ensure_mqtt_connected()) {
        return;
    }

    MQTTClient_yield();
    request_firmware_versions_if_needed();
    (void)publish_chamber_telemetry();
    (void)publish_heartbeat();
    poll_ota_heartbeat_wait();
    expire_pending_acks();
}

void MqttService::subscribe_messages()
{
    auto& bus = middleware::MsgPort::instance();
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Update_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_env_update(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Mcu_Version_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_mcu_version(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Mcu_Ota_Status_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_mcu_ota_status(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Serial_Mcu_Ota_Status_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_serial_mcu_ota_status(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Galvo_Version_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_galvo_version(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Env_Sim_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_env_sim_state(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Print_State_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_print_state(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::UI_Ota_User_Decision_Req,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_ota_user_decision(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::UI_Soc_Ota_Apply_Req,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_soc_ota_apply(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Mqtt_Rtsp_Start_Rsp,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_rtsp_start_rsp(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Mqtt_Rtsp_Stop_Rsp,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_rtsp_stop_rsp(msg); }));
}

bool MqttService::ensure_mqtt_connected()
{
    if(mqtt_connected_ && MQTTClient_isConnected(client_)) {
        return true;
    }

    mqtt_connected_ = false;
    cmd_subscribed_ = false;
    try_reconnect();
    return mqtt_connected_;
}

void MqttService::try_reconnect()
{
    const uint64_t now = monotonic_ms();
    if(last_reconnect_ms_ != 0 &&
       now - last_reconnect_ms_ < kReconnectIntervalMs) {
        return;
    }
    last_reconnect_ms_ = now;
    (void)connect_mqtt();
}

void MqttService::start_activation()
{
    {
        std::lock_guard<std::mutex> lock(activation_mtx_);
        if(activation_running_) {
            return;
        }
        activation_running_ = true;
        activation_result_ready_ = false;
        activation_succeeded_ = false;
        activation_error_.clear();
        activated_credential_ = ActivatedMqttCredential{};
    }

    join_activation_thread();
    const MqttConfig activation_config = cfg_;
    activation_thread_ = std::thread([this, activation_config]() {
        DeviceActivationClient client(activation_config);
        ActivatedMqttCredential credential;
        std::string error;
        const bool succeeded = client.activate(credential, error);
        std::lock_guard<std::mutex> lock(activation_mtx_);
        activation_succeeded_ = succeeded;
        activated_credential_ = std::move(credential);
        activation_error_ = std::move(error);
        activation_running_ = false;
        activation_result_ready_ = true;
    });
}

void MqttService::poll_activation()
{
    bool result_ready = false;
    bool succeeded = false;
    ActivatedMqttCredential credential;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(activation_mtx_);
        result_ready = activation_result_ready_;
        if(result_ready) {
            succeeded = activation_succeeded_;
            credential = activated_credential_;
            error = activation_error_;
            activation_result_ready_ = false;
        }
    }

    if(result_ready) {
        join_activation_thread();
        if(succeeded) {
            std::string save_error;
            if(credential_store_.save(credential, save_error)) {
                cfg_.password = credential.mqtt_password;
                cfg_.broker_uri = normalise_broker_uri(credential.mqtt_endpoint);
                credentials_ready_ = true;
                activation_fail_count_ = 0;
                UPC_LOG_MQTT_SERVICE(Info, "device activation succeeded; MQTT credential saved");
                return;
            }
            error = "激活成功但保存 MQTT 凭据失败: " + save_error;
        }
        ++activation_fail_count_;
        const uint64_t retry_ms = activation_retry_interval_ms(activation_fail_count_);
        next_activation_ms_ = monotonic_ms() + retry_ms;
        UPC_LOG_MQTT_SERVICE(Warn, "device activation failed: %s; retry #%u in %llums",
                             error.c_str(),
                             activation_fail_count_,
                             static_cast<unsigned long long>(retry_ms));
        return;
    }

    bool activation_running = false;
    {
        std::lock_guard<std::mutex> lock(activation_mtx_);
        activation_running = activation_running_;
    }
    if(!activation_running && monotonic_ms() >= next_activation_ms_) {
        UPC_LOG_MQTT_SERVICE(Info, "starting device activation");
        start_activation();
    }
}

void MqttService::join_activation_thread()
{
    if(activation_thread_.joinable()) {
        activation_thread_.join();
    }
}

bool MqttService::connect_mqtt()
{
    if(!mqtt_created_) {
        const int rc = MQTTClient_create(
            &client_,
            cfg_.broker_uri.c_str(),
            cfg_.client_id.c_str(),
            MQTTCLIENT_PERSISTENCE_NONE,
            nullptr);
        if(rc != MQTTCLIENT_SUCCESS) {
            UPC_LOG_MQTT_SERVICE(Error, "MQTT create failed rc=%d", rc);
            return false;
        }
        mqtt_created_ = true;
        MQTTClient_setCallbacks(client_, this, nullptr, &MqttService::on_message_arrived, nullptr);
    }

    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    opts.keepAliveInterval = cfg_.keepalive_sec;
    opts.cleansession = cfg_.clean_session ? 1 : 0;
// #ifdef MQTTVERSION_5
//     opts.MQTTVersion = MQTTVERSION_5;
// #endif
    if(!cfg_.username.empty()) {
        opts.username = cfg_.username.c_str();
    }
    if(!cfg_.password.empty()) {
        opts.password = cfg_.password.c_str();
    }

    const int rc = MQTTClient_connect(client_, &opts);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "MQTT connect failed rc=%d uri=%s",
            rc, cfg_.broker_uri.c_str());
        mqtt_connected_ = false;
        return false;
    }

    mqtt_connected_ = true;
    last_heartbeat_pub_ms_ = 0;
    if(!subscribe_cmd_topic()) {
        disconnect_mqtt();
        return false;
    }
    if(!subscribe_ota_topics()) {
        disconnect_mqtt();
        return false;
    }

    UPC_LOG_MQTT_SERVICE(Info, "MQTT connected uri=%s", cfg_.broker_uri.c_str());

    /* 设备上线后主动上报一次完整状态（协议 §四）：mode 默认 idle，since 取当前时刻 */
    (void)publish_state(static_cast<uint32_t>(middleware::UserHomeStatus::Idle), 0);

    /* 遗留的 SOC 待应用固件在这里补提示：MQTT 比 UI 先启动，放 init() 里发
     * 的话 UI 还没订阅，消息会丢。内部只执行一次。 */
    notify_soc_ota_ready_after_connect();
    return true;
}

bool MqttService::subscribe_cmd_topic()
{
    const std::string topic = cfg_.topic_cmd();
    const int rc = MQTTClient_subscribe(client_, topic.c_str(), 1);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Error, "MQTT subscribe failed rc=%d topic=%s",
            rc, topic.c_str());
        return false;
    }
    cmd_subscribed_ = true;
    UPC_LOG_MQTT_SERVICE(Info, "MQTT subscribed topic=%s", topic.c_str());
    return true;
}

bool MqttService::subscribe_ota_topics()
{
    const std::string notify_topic = cfg_.topic_ota_notify();
    const int rc_notify = MQTTClient_subscribe(client_, notify_topic.c_str(), 1);
    if(rc_notify != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Error, "MQTT subscribe ota notify failed rc=%d topic=%s",
                             rc_notify, notify_topic.c_str());
        return false;
    }
    const std::string manifest_topic = cfg_.topic_ota_manifest_resp();
    const int rc_manifest = MQTTClient_subscribe(client_, manifest_topic.c_str(), 1);
    if(rc_manifest != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Error, "MQTT subscribe ota manifest failed rc=%d topic=%s",
                             rc_manifest, manifest_topic.c_str());
        return false;
    }
    ota_subscribed_ = true;
    UPC_LOG_MQTT_SERVICE(Info, "MQTT subscribed ota topics notify=%s manifest=%s",
                         notify_topic.c_str(), manifest_topic.c_str());
    return true;
}

void MqttService::disconnect_mqtt()
{
    if(mqtt_created_ && MQTTClient_isConnected(client_)) {
        MQTTClient_disconnect(client_, 1000);
    }
    mqtt_connected_ = false;
    cmd_subscribed_ = false;
    ota_subscribed_ = false;
}

bool MqttService::publish_cmd_ack(const std::string& req_id,
                                  const std::string& cmd,
                                  const std::string& result,
                                  const char* reason,
                                  const char* detail_json)
{
    if(req_id.empty()) {
        UPC_LOG_MQTT_SERVICE(Warn, "skip cmd/ack because req_id empty");
        return false;
    }

    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const std::string escaped_device = json_escape(cfg_.device_id);
    const std::string escaped_req = json_escape(req_id);
    const std::string escaped_cmd = json_escape(cmd);
    const std::string escaped_result = json_escape(result);
    const std::string reason_json = reason
        ? ("\"" + json_escape(reason) + "\"")
        : "null";
    const char* detail = detail_json ? detail_json : "{}";

    /* log_collect 的 fileReport 可能很长，用动态缓冲避免截断导致 LOG_REPORT_MISSING。 */
    std::string payload;
    payload.reserve(512 + std::strlen(detail));
    payload += '{';
    payload += "\"v\":1,";
    payload += "\"type\":\"command_ack\",";
    payload += "\"device_id\":\"";
    payload += escaped_device;
    payload += "\",";
    payload += "\"ts\":";
    payload += std::to_string(ts);
    payload += ',';
    payload += "\"seq\":";
    payload += std::to_string(seq);
    payload += ',';
    payload += "\"req_id\":\"";
    payload += escaped_req;
    payload += "\",";
    payload += "\"data\":{";
    payload += "\"cmd\":\"";
    payload += escaped_cmd;
    payload += "\",";
    payload += "\"result\":\"";
    payload += escaped_result;
    payload += "\",";
    payload += "\"reason\":";
    payload += reason_json;
    payload += ',';
    payload += "\"detail\":";
    payload += detail;
    payload += "}}";

    const std::string topic = cfg_.topic_cmd_ack();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload.data();
    msg.payloadlen = static_cast<int>(payload.size());
    msg.qos = cfg_.qos_ack;
    msg.retained = 0;

    debug_log_mqtt_frame("TX", topic, msg.payload,
                         static_cast<size_t>(msg.payloadlen),
                         msg.qos, msg.retained);

    MQTTClient_deliveryToken token;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish cmd/ack fail rc=%d topic=%s",
            rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }
    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT TX cmd/ack queued topic=%s token=%d req_id=%s result=%s",
        topic.c_str(), static_cast<int>(token), req_id.c_str(), result.c_str());
    return true;
}

int MqttService::on_message_arrived(void* context, char* topic_name,
                                    int topic_len, MQTTClient_message* message)
{
    if(!message) {
        if(topic_name) {
            MQTTClient_free(topic_name);
        }
        return 1;
    }

    const std::string topic = topic_from_incoming(topic_name, topic_len);

    /* 原始帧必须先于任何格式校验打印；空载荷/非法 JSON 同样可在日志中看到。 */
    const size_t payload_len = message->payloadlen > 0
        ? static_cast<size_t>(message->payloadlen)
        : 0U;
#if UPC_MQTT_RX_RAW_LOG_ENABLE
    debug_log_mqtt_frame("RX", topic, message->payload,
                         payload_len,
                         message->qos, message->retained);
#endif

    if(context) {
        auto* self = static_cast<MqttService*>(context);
        const std::string payload = (message->payload && payload_len > 0)
            ? std::string(static_cast<const char*>(message->payload), payload_len)
            : std::string{};
        if(topic == self->cfg_.topic_cmd()) {
            self->handle_command_message(payload);
        } else if(topic == self->cfg_.topic_ota_notify()) {
            self->handle_ota_notify(payload);
        } else if(topic == self->cfg_.topic_ota_manifest_resp()) {
            self->handle_ota_manifest_response(payload);
        } else {
            UPC_LOG_MQTT_SERVICE(Debug, "MQTT RX ignored topic=%s", topic.c_str());
        }
    } else {
        UPC_LOG_MQTT_SERVICE(Warn,
            "MQTT RX frame dropped because callback context is null");
    }

    MQTTClient_freeMessage(&message);
    if(topic_name) {
        MQTTClient_free(topic_name);
    }
    return 1;
}

uint64_t MqttService::monotonic_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

int64_t MqttService::unix_ms()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string MqttService::read_boot_id()
{
    std::ifstream in("/proc/sys/kernel/random/boot_id");
    std::string boot_id;
    if(std::getline(in, boot_id) && !boot_id.empty()) {
        return boot_id;
    }
    const long long now = static_cast<long long>(unix_ms());
    return std::string("boot-") + std::to_string(now);
}

int64_t MqttService::read_uptime_s()
{
    std::ifstream in("/proc/uptime");
    double uptime = 0.0;
    if(in >> uptime && uptime >= 0.0) {
        return static_cast<int64_t>(uptime);
    }
    return 0;
}

int MqttService::bars_to_rssi_dbm(int bars, bool connected)
{
    if(!connected) {
        return -127;
    }
    if(bars >= 3) {
        return -55;
    }
    if(bars == 2) {
        return -67;
    }
    if(bars == 1) {
        return -78;
    }
    return -90;
}

std::string MqttService::format_mcu_version(const middleware::McuVersionPayload& p)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u.%u",
                  static_cast<unsigned>(p.major),
                  static_cast<unsigned>(p.minor),
                  static_cast<unsigned>(p.patch),
                  static_cast<unsigned>(p.laser_type),
                  static_cast<unsigned>(p.z_motor_type));
    return std::string(buf);
}

std::string MqttService::format_galvo_version(uint32_t fw_version_le)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u",
                  static_cast<unsigned>((fw_version_le >> 16) & 0xFFu),
                  static_cast<unsigned>((fw_version_le >> 8) & 0xFFu),
                  static_cast<unsigned>(fw_version_le & 0xFFu));
    return std::string(buf);
}

bool MqttService::parse_mcu_version_text(const std::string& text,
                                         middleware::McuVersionPayload& out)
{
    out = middleware::McuVersionPayload{};
    std::string v = text;
    if(!v.empty() && (v[0] == 'V' || v[0] == 'v')) {
        v.erase(v.begin());
    }
    unsigned a = 0, b = 0, c = 0, d = 0, e = 0;
    if(std::sscanf(v.c_str(), "%u.%u.%u.%u.%u", &a, &b, &c, &d, &e) != 5) {
        return false;
    }
    if(a > 255 || b > 255 || c > 255 || d > 255 || e > 255) {
        return false;
    }
    out.valid = 1;
    out.major = static_cast<uint8_t>(a);
    out.minor = static_cast<uint8_t>(b);
    out.patch = static_cast<uint8_t>(c);
    out.laser_type = static_cast<uint8_t>(d);
    out.z_motor_type = static_cast<uint8_t>(e);
    return true;
}

bool MqttService::parse_galvo_version_text(const std::string& text,
                                           middleware::GalvoVersionPayload& out)
{
    out = middleware::GalvoVersionPayload{};
    std::string v = text;
    if(!v.empty() && (v[0] == 'V' || v[0] == 'v')) {
        v.erase(v.begin());
    }
    unsigned major = 0, minor = 0, patch = 0;
    if(std::sscanf(v.c_str(), "%u.%u.%u", &major, &minor, &patch) != 3) {
        return false;
    }
    if(major > 255 || minor > 255 || patch > 255) {
        return false;
    }
    out.valid = 1;
    out.firmware_id = 1;
    out.fw_version_le =
        ((major & 0xFFu) << 16) |
        ((minor & 0xFFu) << 8) |
        (patch & 0xFFu);
    return true;
}

bool MqttService::json_extract_string(const std::string& json, const char* key,
                                      std::string& out)
{
    out.clear();
    if(!key || !key[0]) {
        return false;
    }
    const std::string token = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(token);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', key_pos + token.size());
    if(pos == std::string::npos) {
        return false;
    }
    ++pos;
    while(pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if(pos >= json.size() || json[pos] != '"') {
        return false;
    }
    ++pos;
    std::string value;
    bool escaped = false;
    for(; pos < json.size(); ++pos) {
        const char c = json[pos];
        if(escaped) {
            value.push_back(c);
            escaped = false;
            continue;
        }
        if(c == '\\') {
            escaped = true;
            continue;
        }
        if(c == '"') {
            out = value;
            return true;
        }
        value.push_back(c);
    }
    return false;
}

bool MqttService::json_extract_int(const std::string& json, const char* key, int& out)
{
    int64_t v = 0;
    if(!json_extract_int64(json, key, v)) {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool MqttService::json_extract_int64(const std::string& json, const char* key, int64_t& out)
{
    const std::string token = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(token);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', key_pos + token.size());
    if(pos == std::string::npos) {
        return false;
    }
    ++pos;
    while(pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if(pos >= json.size()) {
        return false;
    }
    size_t end = pos;
    if(json[end] == '-') {
        ++end;
    }
    while(end < json.size() && std::isdigit(static_cast<unsigned char>(json[end]))) {
        ++end;
    }
    if(end == pos || (end == pos + 1 && json[pos] == '-')) {
        return false;
    }
    out = std::strtoll(json.substr(pos, end - pos).c_str(), nullptr, 10);
    return true;
}

bool MqttService::json_extract_string_array(const std::string& json, const char* key,
                                            std::vector<std::string>& out)
{
    out.clear();
    if(!key || !key[0]) {
        return false;
    }
    const std::string token = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(token);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', key_pos + token.size());
    if(pos == std::string::npos) {
        return false;
    }
    ++pos;
    while(pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if(pos >= json.size() || json[pos] != '[') {
        return false;
    }
    ++pos;
    for(; pos < json.size();) {
        while(pos < json.size() &&
              (std::isspace(static_cast<unsigned char>(json[pos])) || json[pos] == ',')) {
            ++pos;
        }
        if(pos >= json.size()) {
            return false;
        }
        if(json[pos] == ']') {
            return true;
        }
        if(json[pos] != '"') {
            return false;
        }
        ++pos;
        std::string value;
        bool escaped = false;
        for(; pos < json.size(); ++pos) {
            const char c = json[pos];
            if(escaped) {
                value.push_back(c);
                escaped = false;
                continue;
            }
            if(c == '\\') {
                escaped = true;
                continue;
            }
            if(c == '"') {
                out.push_back(std::move(value));
                ++pos;
                break;
            }
            value.push_back(c);
        }
    }
    return false;
}

bool MqttService::json_extract_object(const std::string& json, const char* key,
                                      std::string& out)
{
    out.clear();
    if(!key || !key[0]) {
        return false;
    }

    const std::string token = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(token);
    if(key_pos == std::string::npos) {
        return false;
    }

    size_t pos = json.find(':', key_pos + token.size());
    if(pos == std::string::npos) {
        return false;
    }
    ++pos;
    while(pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if(pos >= json.size() || json[pos] != '{') {
        return false;
    }

    const size_t object_begin = pos;
    size_t depth = 0;
    bool in_string = false;
    bool escaped = false;
    for(; pos < json.size(); ++pos) {
        const char c = json[pos];
        if(in_string) {
            if(escaped) {
                escaped = false;
            } else if(c == '\\') {
                escaped = true;
            } else if(c == '"') {
                in_string = false;
            }
            continue;
        }

        if(c == '"') {
            in_string = true;
        } else if(c == '{') {
            ++depth;
        } else if(c == '}') {
            if(depth == 0) {
                return false;
            }
            --depth;
            if(depth == 0) {
                out.assign(json, object_begin, pos - object_begin + 1);
                return true;
            }
        }
    }
    return false;
}

bool MqttService::json_extract_bool(const std::string& json, const char* key, bool& out)
{
    const std::string token = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(token);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', key_pos + token.size());
    if(pos == std::string::npos) {
        return false;
    }
    ++pos;
    while(pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if(json.compare(pos, 4, "true") == 0) {
        out = true;
        return true;
    }
    if(json.compare(pos, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

bool MqttService::json_extract_array_objects(const std::string& json, const char* key,
                                             std::vector<std::string>& out)
{
    out.clear();
    const std::string token = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(token);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t pos = json.find(':', key_pos + token.size());
    if(pos == std::string::npos) {
        return false;
    }
    ++pos;
    while(pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if(pos >= json.size() || json[pos] != '[') {
        return false;
    }
    ++pos;
    for(; pos < json.size();) {
        while(pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
            ++pos;
        }
        if(pos >= json.size()) {
            return false;
        }
        if(json[pos] == ']') {
            return true;
        }
        if(json[pos] != '{') {
            ++pos;
            continue;
        }
        const size_t begin = pos;
        size_t depth = 0;
        bool in_string = false;
        bool escaped = false;
        for(; pos < json.size(); ++pos) {
            const char c = json[pos];
            if(in_string) {
                if(escaped) {
                    escaped = false;
                } else if(c == '\\') {
                    escaped = true;
                } else if(c == '"') {
                    in_string = false;
                }
                continue;
            }
            if(c == '"') {
                in_string = true;
            } else if(c == '{') {
                ++depth;
            } else if(c == '}') {
                if(depth == 0) {
                    return false;
                }
                --depth;
                if(depth == 0) {
                    out.emplace_back(json.substr(begin, pos - begin + 1));
                    ++pos;
                    break;
                }
            }
        }
        while(pos < json.size() &&
              (std::isspace(static_cast<unsigned char>(json[pos])) || json[pos] == ',')) {
            ++pos;
        }
    }
    return false;
}

std::string MqttService::topic_from_incoming(const char* topic_name, int topic_len)
{
    if(!topic_name) {
        return {};
    }
    if(topic_len > 0) {
        return std::string(topic_name, static_cast<size_t>(topic_len));
    }
    return std::string(topic_name);
}

bool MqttService::map_approval_status(const std::string& text,
                                      middleware::OtaApprovalStatus& out)
{
    std::string v = text;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if(v == "PENDING_APPROVAL") {
        out = middleware::OtaApprovalStatus::PendingApproval;
        return true;
    }
    if(v == "APPROVED") {
        out = middleware::OtaApprovalStatus::Approved;
        return true;
    }
    if(v == "READY") {
        out = middleware::OtaApprovalStatus::Ready;
        return true;
    }
    out = middleware::OtaApprovalStatus::Unknown;
    return false;
}

std::string MqttService::json_escape(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for(char c : text) {
        switch(c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

} // namespace upc::mqtt
