#include "mqtt_service.hpp"

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
#include <sys/types.h>
#include <sys/wait.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/evp.h>

namespace upc::mqtt {

namespace {

constexpr uint64_t kReconnectIntervalMs = 3000;
constexpr uint64_t kActivationRetryIntervalMs = 5000;
constexpr uint64_t kTelemetryPrintingIntervalMs = 2000;
constexpr uint64_t kTelemetryIdleIntervalMs = 30000;
constexpr uint64_t kFirmwareQueryIntervalMs = 10000;

constexpr const char* kCmdSetLight = "set_light";
constexpr const char* kCmdStartRtspPush = "start_rtsp_push";
constexpr const char* kCmdStopRtspPush = "stop_rtsp_push";
constexpr const char* kCmdRejectReasonInvalidState = "invalid_state";
constexpr const char* kCmdRejectReasonEncoderError = "encoder_error";
constexpr uint64_t kRtspAckTimeoutMs = 8000;
constexpr const char* kImageTmpDir = "/home/image/tmp";
constexpr const char* kImageSocDir = "/home/image/soc";
constexpr const char* kImageMcuDir = "/home/image/mcu";
constexpr const char* kImageGlavoDir = "/home/image/glavo";

#ifndef UPC_SW_VERSION
#define UPC_SW_VERSION "unknown"
#endif

#if UPC_DEBUG && UPC_MOD_MQTT_SERVICE
/**
 * @brief 打印 MQTT 收发原始帧。
 *
 * JSON 可打印字符保持原样，控制字符/二进制字节转义为 \xNN；分块限制单条日志长度，
 * 避免超过 rs_log 的消息缓冲区。该函数完全受 UPC_MOD_MQTT_SERVICE 控制。
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
        UPC_LOG_MQTT_SERVICE(Debug, "MQTT %s payload=<empty>", dir);
        return;
    }

    constexpr size_t kMaxEncodedChunk = 1200;
    const auto* bytes = static_cast<const unsigned char*>(payload);
    size_t offset = 0;
    while(offset < payload_len) {
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
            "MQTT %s payload[%zu..%zu]=\"%s\"",
            dir, begin, offset, encoded.c_str());
    }
}
#else
inline void debug_log_mqtt_frame(const char*, const std::string&, const void*,
                                 size_t, int, int)
{
}
#endif

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string path;
    uint16_t port = 0;
};

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
        "Host: " + parsed.host + "\r\n"
        "Connection: close\r\n"
        "User-Agent: snowpod-mqtt-ota/1.0\r\n\r\n";
    if(!write_all(request.data(), request.size())) {
        err = "http_send_failed";
        if(ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
        ::close(fd);
        return false;
    }

    std::ofstream out(out_file, std::ios::binary | std::ios::trunc);
    if(!out) {
        err = "open_tmp_failed";
        if(ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
        ::close(fd);
        return false;
    }

    std::string header;
    std::array<char, 4096> buf{};
    bool header_done = false;
    size_t content_length = 0;
    size_t received = 0;

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
            if(status_line.find("200") == std::string::npos) {
                err = "http_status_not_200";
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
                }
            }
            if(!body.empty()) {
                out.write(body.data(), static_cast<std::streamsize>(body.size()));
                received += body.size();
                if(on_progress) {
                    on_progress(received, content_length);
                }
            }
            continue;
        }
        out.write(buf.data(), n);
        received += static_cast<size_t>(n);
        if(on_progress) {
            on_progress(received, content_length);
        }
    }

    out.flush();
    out.close();

    if(ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
    }
    ::close(fd);
    if(!err.empty()) {
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
    if(component == "glavo") {
        return kImageGlavoDir;
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

bool archive_entry_safe_flat(const std::string& entry)
{
    if(entry.empty()) {
        return false;
    }
    if(entry.find('/') != std::string::npos || entry.find('\\') != std::string::npos) {
        return false;
    }
    if(entry.find("..") != std::string::npos) {
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
    if(name.size() < 5) {
        return false;
    }
    const std::string upper = to_upper_ascii(name);
    return upper.rfind(".BIN") == upper.size() - 4;
}

} // namespace

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
        middleware::MsgType::GLAVO_Galvo_Version_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_galvo_version(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::GLAVO_Debug_Env_Sim_Ntf,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_env_sim_state(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::UI_Ota_User_Decision_Req,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_ota_user_decision(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Mqtt_Rtsp_Start_Rsp,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_rtsp_start_rsp(msg); }));
    subs_.push_back(bus.subscribe(
        middleware::MsgType::APP_Mqtt_Rtsp_Stop_Rsp,
        middleware::ModuleId::APP_MQTT,
        [this](const middleware::Message& msg) { on_rtsp_stop_rsp(msg); }));
}

void MqttService::on_env_update(const middleware::Message& msg)
{
    middleware::EnvSnapshotPayload p{};
    if(!read_env_payload(msg, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid APP_Update_Ntf payload");
        return;
    }
    UPC_LOG_MQTT_SERVICE(Debug,
        "IPC RX APP_Update_Ntf status=%u pm25=%.3f o2=%.3f%% temp=%.3fC "
        "humidity=%.3f%% pressure=%.3fkPa",
        static_cast<unsigned>(p.status),
        static_cast<double>(p.pm25_cabin),
        static_cast<double>(p.oxygen_percent),
        static_cast<double>(p.temperature_c),
        static_cast<double>(p.humidity_percent),
        static_cast<double>(p.pressure_kpa));
    cache_telemetry(p);
}

void MqttService::on_mcu_version(const middleware::Message& msg)
{
    middleware::McuVersionPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid GLAVO_Mcu_Version_Ntf payload");
        return;
    }
    bool publish_now = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        fw_cache_.mcu = p;
        fw_cache_.mcu_valid = (p.valid != 0);
        if(ota_hb_wait_.active && ota_hb_wait_.need_mcu) {
            ota_hb_wait_.got_mcu = true;
            if((!ota_hb_wait_.need_mcu || ota_hb_wait_.got_mcu) &&
               (!ota_hb_wait_.need_galvo || ota_hb_wait_.got_galvo)) {
                ota_hb_wait_.active = false;
                publish_now = true;
            }
        }
    }
    if(publish_now) {
        last_heartbeat_pub_ms_ = 0;
        (void)publish_heartbeat();
    }
}

void MqttService::on_galvo_version(const middleware::Message& msg)
{
    middleware::GalvoVersionPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid GLAVO_Galvo_Version_Ntf payload");
        return;
    }
    bool publish_now = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        fw_cache_.galvo = p;
        fw_cache_.galvo_valid = (p.valid != 0);
        if(ota_hb_wait_.active && ota_hb_wait_.need_galvo) {
            ota_hb_wait_.got_galvo = true;
            if((!ota_hb_wait_.need_mcu || ota_hb_wait_.got_mcu) &&
               (!ota_hb_wait_.need_galvo || ota_hb_wait_.got_galvo)) {
                ota_hb_wait_.active = false;
                publish_now = true;
            }
        }
    }
    if(publish_now) {
        last_heartbeat_pub_ms_ = 0;
        (void)publish_heartbeat();
    }
}

void MqttService::on_env_sim_state(const middleware::Message& msg)
{
    middleware::DebugOnOffPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid GLAVO_Debug_Env_Sim_Ntf payload");
        return;
    }
    std::lock_guard<std::mutex> lock(data_mtx_);
    env_sim_on_ = (p.on != 0);
}

void MqttService::on_ota_user_decision(const middleware::Message& msg)
{
    middleware::OtaDecisionReqPayload p{};
    if(!read_inline_pod(msg.payload, p)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid UI_Ota_User_Decision_Req payload");
        return;
    }
    p.task_id[sizeof(p.task_id) - 1] = '\0';
    if(p.task_id[0] == '\0') {
        UPC_LOG_MQTT_SERVICE(Warn, "ignore OTA decision with empty task_id");
        return;
    }
    if(p.approve == 0) {
        bool required = false;
        {
            std::lock_guard<std::mutex> lock(ota_mtx_);
            required = pending_ota_notify_.required &&
                       pending_ota_notify_.task_id == p.task_id;
        }
        if(required) {
            UPC_LOG_MQTT_SERVICE(Warn, "ignore cancel for required OTA task_id=%s", p.task_id);
            return;
        }
        UPC_LOG_MQTT_SERVICE(Info, "OTA notify cancelled by UI task_id=%s", p.task_id);
        return;
    }
    (void)publish_manifest_request(p.task_id);
}

void MqttService::cache_telemetry(const middleware::EnvSnapshotPayload& p)
{
    std::lock_guard<std::mutex> lock(data_mtx_);
    /* 环境指标全为哨兵 <0（上电无数据 / Env Sim 关闭清缓存）→ 停遥测直到下一帧有效数据 */
    const bool env_invalid =
        (p.pm25_cabin < 0.0f) &&
        (p.oxygen_percent < 0.0f) &&
        (p.temperature_c < 0.0f);
    if(env_invalid) {
        latest_.wifi_connected = p.wifi_connected != 0;
        latest_.signal_bars = p.signal_bars;
        latest_.has_value = false;
        return;
    }
    latest_.pm25 = p.pm25_cabin;
    latest_.oxygen_percent = p.oxygen_percent;
    latest_.temperature_c = p.temperature_c;
    latest_.status = p.status;
    latest_.wifi_connected = p.wifi_connected != 0;
    latest_.signal_bars = p.signal_bars;
    latest_.has_value = true;
}

bool MqttService::read_env_payload(const middleware::Message& msg,
                                   middleware::EnvSnapshotPayload& out) const
{
    if(msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length < sizeof(out)) {
        return false;
    }
    std::memcpy(&out, msg.payload.data.bytes.data(), sizeof(out));
    return true;
}

void MqttService::request_firmware_versions_if_needed()
{
    bool need_mcu = false;
    bool need_galvo = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        need_mcu = !fw_cache_.mcu_valid;
        need_galvo = !fw_cache_.galvo_valid;
    }
    if(!need_mcu && !need_galvo) {
        return;
    }

    const uint64_t now = monotonic_ms();
    if(last_fw_query_ms_ != 0 && now - last_fw_query_ms_ < kFirmwareQueryIntervalMs) {
        return;
    }
    last_fw_query_ms_ = now;

    auto& port = middleware::MsgPort::instance();
    if(need_mcu) {
        middleware::Message req(middleware::MsgType::UI_Mcu_Version_Req,
                                middleware::ModuleId::APP_MQTT,
                                middleware::ModuleId::APP_GLAVO);
        (void)port.publish_async(req);
    }
    if(need_galvo) {
        middleware::Message req(middleware::MsgType::UI_Galvo_Version_Req,
                                middleware::ModuleId::APP_MQTT,
                                middleware::ModuleId::APP_GLAVO);
        (void)port.publish_async(req);
    }
}

void MqttService::request_firmware_versions_force(bool need_mcu, bool need_galvo)
{
    auto& port = middleware::MsgPort::instance();
    if(need_mcu) {
        middleware::Message req(middleware::MsgType::UI_Mcu_Version_Req,
                                middleware::ModuleId::APP_MQTT,
                                middleware::ModuleId::APP_GLAVO);
        (void)port.publish_async(req);
    }
    if(need_galvo) {
        middleware::Message req(middleware::MsgType::UI_Galvo_Version_Req,
                                middleware::ModuleId::APP_MQTT,
                                middleware::ModuleId::APP_GLAVO);
        (void)port.publish_async(req);
    }
}

void MqttService::poll_ota_heartbeat_wait()
{
    bool timeout_publish = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        if(ota_hb_wait_.active && monotonic_ms() >= ota_hb_wait_.deadline_ms) {
            ota_hb_wait_.active = false;
            timeout_publish = true;
        }
    }
    if(timeout_publish) {
        last_heartbeat_pub_ms_ = 0;
        (void)publish_heartbeat();
    }
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
                UPC_LOG_MQTT_SERVICE(Info, "device activation succeeded; MQTT credential saved");
                return;
            }
            error = "激活成功但保存 MQTT 凭据失败: " + save_error;
        }
        next_activation_ms_ = monotonic_ms() + kActivationRetryIntervalMs;
        UPC_LOG_MQTT_SERVICE(Warn, "device activation failed: %s; retry in %llums",
                             error.c_str(),
                             static_cast<unsigned long long>(kActivationRetryIntervalMs));
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

bool MqttService::publish_chamber_telemetry()
{
    ChamberTelemetry snap{};
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        snap = latest_;
    }
    if(!snap.has_value) {
        return false;
    }

    const bool printing = snap.status ==
        static_cast<uint32_t>(middleware::UserHomeStatus::Printing);
    const uint64_t interval_ms = printing
        ? kTelemetryPrintingIntervalMs
        : kTelemetryIdleIntervalMs;

    const uint64_t now = monotonic_ms();
    if(last_telemetry_pub_ms_ != 0 &&
       now - last_telemetry_pub_ms_ < interval_ms) {
        return false;
    }
    last_telemetry_pub_ms_ = now;

    int o2_ppm = static_cast<int>(std::lround(static_cast<double>(snap.oxygen_percent) * 10000.0));
    if(o2_ppm < 0) {
        o2_ppm = 0;
    }

    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const std::string escaped_device = json_escape(cfg_.device_id);

    char payload[768];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"telemetry\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"data\":{"
            "\"group\":\"chamber\","
            "\"pm25\":%.3f,"
            "\"o2_ppm\":%d,"
            "\"dp_pa\":null,"
            "\"ir_temp_c\":null,"
            "\"ntc_c\":%.3f,"
            "\"smoke\":null"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        static_cast<double>(snap.pm25),
        o2_ppm,
        static_cast<double>(snap.temperature_c));

    const std::string topic = cfg_.topic_telemetry_chamber();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
    msg.qos = cfg_.qos_telemetry;
    msg.retained = 0;

    debug_log_mqtt_frame("TX", topic, msg.payload,
                         static_cast<size_t>(msg.payloadlen),
                         msg.qos, msg.retained);

    MQTTClient_deliveryToken token;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish telemetry fail rc=%d topic=%s",
            rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }
    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT TX telemetry queued topic=%s token=%d",
        topic.c_str(), static_cast<int>(token));
    return true;
}

bool MqttService::publish_heartbeat()
{
    const uint64_t now = monotonic_ms();
    const uint64_t interval_ms =
        static_cast<uint64_t>(cfg_.heartbeat_interval_sec) * 1000ULL;
    if(last_heartbeat_pub_ms_ != 0 &&
       now - last_heartbeat_pub_ms_ < interval_ms) {
        return false;
    }

    bool wifi_connected = false;
    int signal_bars = -1;
    middleware::McuVersionPayload mcu_ver{};
    middleware::GalvoVersionPayload galvo_ver{};
    bool mcu_valid = false;
    bool galvo_valid = false;
    bool env_sim_on = false;
    std::string soc_override;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        wifi_connected = latest_.wifi_connected;
        signal_bars = latest_.signal_bars;
        mcu_ver = fw_cache_.mcu;
        galvo_ver = fw_cache_.galvo;
        mcu_valid = fw_cache_.mcu_valid;
        galvo_valid = fw_cache_.galvo_valid;
        env_sim_on = env_sim_on_;
        soc_override = soc_fw_override_;
    }

    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    const long long uptime_s = read_uptime_s();
    const int rssi = bars_to_rssi_dbm(signal_bars, wifi_connected);
    const char* network_type = wifi_connected ? "wifi" : "none";
    const std::string escaped_device = json_escape(cfg_.device_id);
    const std::string escaped_boot = json_escape(boot_id_);
    const std::string soc_fw = (env_sim_on && !soc_override.empty())
        ? soc_override
        : std::string(UPC_SW_VERSION);
    const std::string escaped_soc_fw = json_escape(soc_fw);
    const std::string escaped_mcu_fw = json_escape(
        mcu_valid ? format_mcu_version(mcu_ver) : std::string("unknown"));
    const std::string escaped_galvo_fw = json_escape(
        galvo_valid ? format_galvo_version(galvo_ver.fw_version_le) : std::string("unknown"));

    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"heartbeat\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"data\":{"
            "\"boot_id\":\"%s\","
            "\"uptime_s\":%lld,"
            "\"firmwares\":["
                "{\"firmware_type\":\"soc\",\"version\":\"%s\"},"
                "{\"firmware_type\":\"mcu\",\"version\":\"%s\"},"
                "{\"firmware_type\":\"galvo\",\"version\":\"%s\"}"
            "],"
            "\"battery\":null,"
            "\"network\":{\"type\":\"%s\",\"rssi\":%d}"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        escaped_boot.c_str(),
        uptime_s,
        escaped_soc_fw.c_str(),
        escaped_mcu_fw.c_str(),
        escaped_galvo_fw.c_str(),
        network_type,
        rssi);

    const std::string topic = cfg_.topic_heartbeat();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
    msg.qos = cfg_.qos_heartbeat;
    msg.retained = 0;

    debug_log_mqtt_frame("TX", topic, msg.payload,
                         static_cast<size_t>(msg.payloadlen),
                         msg.qos, msg.retained);

    MQTTClient_deliveryToken token;
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish heartbeat fail rc=%d topic=%s",
                             rc, topic.c_str());
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        return false;
    }

    last_heartbeat_pub_ms_ = now;
    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT TX heartbeat queued topic=%s token=%d",
        topic.c_str(), static_cast<int>(token));
    return true;
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

    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"command_ack\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"req_id\":\"%s\","
        "\"data\":{"
            "\"cmd\":\"%s\","
            "\"result\":\"%s\","
            "\"reason\":%s,"
            "\"detail\":%s"
        "}"
        "}",
        escaped_device.c_str(),
        ts,
        seq,
        escaped_req.c_str(),
        escaped_cmd.c_str(),
        escaped_result.c_str(),
        reason_json.c_str(),
        detail);

    const std::string topic = cfg_.topic_cmd_ack();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
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

void MqttService::handle_command_message(const std::string& payload)
{
    std::string req_id;
    std::string cmd_name;
    std::string data_json;
    std::string reject_reason;
    if(!parse_common_command(payload, req_id, cmd_name, data_json, reject_reason)) {
        UPC_LOG_MQTT_SERVICE(Debug,
            "MQTT RX command rejected req_id=%s cmd=%s reason=%s",
            req_id.c_str(), cmd_name.c_str(),
            reject_reason.empty() ? kCmdRejectReasonInvalidState
                                  : reject_reason.c_str());
        if(!req_id.empty()) {
            const std::string ack_cmd = cmd_name.empty() ? "command" : cmd_name;
            (void)publish_cmd_ack(req_id, ack_cmd, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str());
        }
        return;
    }

    if(cmd_name == kCmdSetLight) {
        LightCommand light{};
        if(!parse_light_command(data_json, req_id, light, reject_reason)) {
            (void)publish_cmd_ack(req_id, kCmdSetLight, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str());
            return;
        }
        UPC_LOG_MQTT_SERVICE(Debug,
            "MQTT RX command parsed req_id=%s cmd=%s mode=%s brightness=%d",
            light.req_id.c_str(), kCmdSetLight, light.mode.c_str(), light.brightness);
        if(dispatch_set_light_to_glavo(light)) {
            (void)publish_cmd_ack(light.req_id, kCmdSetLight, "accepted", nullptr);
        } else {
            (void)publish_cmd_ack(light.req_id, kCmdSetLight, "error", nullptr);
        }
        return;
    }

    if(cmd_name == kCmdStartRtspPush) {
        RtspCommand rtsp{};
        if(!parse_rtsp_start_command(data_json, req_id, rtsp, reject_reason)) {
            (void)publish_cmd_ack(req_id, kCmdStartRtspPush, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str());
            return;
        }
        if(!track_pending_ack(rtsp.req_id, kCmdStartRtspPush)) {
            (void)publish_cmd_ack(rtsp.req_id, kCmdStartRtspPush, "rejected",
                                  kCmdRejectReasonInvalidState);
            return;
        }
        if(!dispatch_rtsp_start_to_media(rtsp)) {
            {
                std::lock_guard<std::mutex> lock(pending_mtx_);
                pending_acks_.erase(rtsp.req_id);
            }
            (void)publish_cmd_ack(rtsp.req_id, kCmdStartRtspPush, "error",
                                  kCmdRejectReasonEncoderError);
        }
        return;
    }

    if(cmd_name == kCmdStopRtspPush) {
        RtspCommand rtsp{};
        if(!parse_rtsp_stop_command(data_json, req_id, rtsp, reject_reason)) {
            (void)publish_cmd_ack(req_id, kCmdStopRtspPush, "rejected",
                                  reject_reason.empty()
                                      ? kCmdRejectReasonInvalidState
                                      : reject_reason.c_str());
            return;
        }
        if(!track_pending_ack(rtsp.req_id, kCmdStopRtspPush)) {
            (void)publish_cmd_ack(rtsp.req_id, kCmdStopRtspPush, "rejected",
                                  kCmdRejectReasonInvalidState);
            return;
        }
        if(!dispatch_rtsp_stop_to_media(rtsp)) {
            {
                std::lock_guard<std::mutex> lock(pending_mtx_);
                pending_acks_.erase(rtsp.req_id);
            }
            (void)publish_cmd_ack(rtsp.req_id, kCmdStopRtspPush, "error",
                                  kCmdRejectReasonEncoderError);
        }
        return;
    }

    UPC_LOG_MQTT_SERVICE(Debug,
        "MQTT RX command ignored req_id=%s cmd=%s",
        req_id.c_str(), cmd_name.c_str());
    (void)publish_cmd_ack(req_id, cmd_name, "rejected",
                          kCmdRejectReasonInvalidState);
}

bool MqttService::parse_common_command(const std::string& payload,
                                       std::string& req_id,
                                       std::string& cmd,
                                       std::string& data_json,
                                       std::string& reject_reason) const
{
    req_id.clear();
    cmd.clear();
    data_json.clear();
    reject_reason.clear();

    std::string type;
    if(json_extract_string(payload, "type", type) && type != "command") {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    if(!json_extract_string(payload, "req_id", req_id)) {
        (void)json_extract_string(payload, "cmdId", req_id);
    }
    if(req_id.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    std::string device_id;
    if(json_extract_string(payload, "device_id", device_id) &&
       device_id != cfg_.device_id) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    if(!json_extract_object(payload, "data", data_json)) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_string(data_json, "cmd", cmd) || cmd.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    return true;
}

bool MqttService::parse_light_command(const std::string& data_json,
                                      const std::string& req_id,
                                      LightCommand& out,
                                      std::string& reject_reason) const
{
    reject_reason.clear();

    std::string zone;
    if(!json_extract_string(data_json, "zone", zone) || zone != "chamber") {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    std::string mode;
    if(!json_extract_string(data_json, "mode", mode) ||
       (mode != "on" && mode != "off")) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }

    int brightness = -1;
    if(json_extract_int(data_json, "brightness", brightness)) {
        if(brightness < 0 || brightness > 100) {
            reject_reason = kCmdRejectReasonInvalidState;
            return false;
        }
    }

    out.req_id = req_id;
    out.mode = mode;
    out.brightness = brightness;
    return true;
}

bool MqttService::parse_rtsp_start_command(const std::string& data_json,
                                           const std::string& req_id,
                                           RtspCommand& out,
                                           std::string& reject_reason) const
{
    reject_reason.clear();
    out = RtspCommand{};
    out.req_id = req_id;

    if(!json_extract_string(data_json, "stream_type", out.stream_type) ||
       out.stream_type.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_string(data_json, "push_url", out.push_url) ||
       out.push_url.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    (void)json_extract_string(data_json, "push_token", out.push_token);
    (void)json_extract_string(data_json, "recording_id", out.recording_id);

    int seek_s = 0;
    if(json_extract_int(data_json, "seek_s", seek_s) && seek_s >= 0) {
        out.seek_s = static_cast<uint32_t>(seek_s);
    }
    int bitrate = 0;
    if(json_extract_int(data_json, "max_bitrate_kbps", bitrate) && bitrate > 0) {
        out.max_bitrate_kbps = static_cast<uint32_t>(bitrate);
    }

    std::string transport;
    if(json_extract_string(data_json, "transport", transport) &&
       !transport.empty() && transport != "tcp") {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    return true;
}

bool MqttService::parse_rtsp_stop_command(const std::string& data_json,
                                          const std::string& req_id,
                                          RtspCommand& out,
                                          std::string& reject_reason) const
{
    reject_reason.clear();
    out = RtspCommand{};
    out.req_id = req_id;
    if(!json_extract_string(data_json, "stream_type", out.stream_type) ||
       out.stream_type.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    return true;
}

bool MqttService::dispatch_set_light_to_glavo(const LightCommand& cmd)
{
    middleware::DebugDevicePayload p{};
    p.target = static_cast<uint8_t>(middleware::DebugDeviceTarget::Light);
    p.on = cmd.mode == "on" ? 1 : 0;

    middleware::Message msg(middleware::MsgType::UI_Debug_Device_Req,
        middleware::ModuleId::APP_MQTT,
        middleware::ModuleId::APP_GLAVO);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));

    const bool ok = middleware::MsgPort::instance().publish_async(msg);
    if(!ok) {
        UPC_LOG_MQTT_SERVICE(Warn, "dispatch set_light failed");
    }
    return ok;
}

bool MqttService::dispatch_rtsp_start_to_media(const RtspCommand& cmd)
{
    middleware::RtspPushStartReqPayload p{};
    std::snprintf(p.req_id, sizeof(p.req_id), "%s", cmd.req_id.c_str());
    std::snprintf(p.push_url, sizeof(p.push_url), "%s", cmd.push_url.c_str());
    std::snprintf(p.push_token, sizeof(p.push_token), "%s", cmd.push_token.c_str());
    std::snprintf(p.stream_type, sizeof(p.stream_type), "%s",
                  cmd.stream_type.c_str());
    std::snprintf(p.recording_id, sizeof(p.recording_id), "%s",
                  cmd.recording_id.c_str());
    p.seek_s = cmd.seek_s;
    p.max_bitrate_kbps = cmd.max_bitrate_kbps;
    p.transport = 0;

    middleware::Message msg(middleware::MsgType::MQTT_Media_Rtsp_Start_Req,
        middleware::ModuleId::APP_MQTT,
        middleware::ModuleId::APP_MEDIA);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    const bool ok = middleware::MsgPort::instance().publish_async(msg);
    if(!ok) {
        UPC_LOG_MQTT_SERVICE(Warn, "dispatch start_rtsp_push failed");
    } else {
        UPC_LOG_MQTT_SERVICE(Info,
            "dispatch start_rtsp_push req_id=%s stream_type=%s",
            cmd.req_id.c_str(), cmd.stream_type.c_str());
    }
    return ok;
}

bool MqttService::dispatch_rtsp_stop_to_media(const RtspCommand& cmd)
{
    middleware::RtspPushStopReqPayload p{};
    std::snprintf(p.req_id, sizeof(p.req_id), "%s", cmd.req_id.c_str());
    std::snprintf(p.stream_type, sizeof(p.stream_type), "%s",
                  cmd.stream_type.c_str());

    middleware::Message msg(middleware::MsgType::MQTT_Media_Rtsp_Stop_Req,
        middleware::ModuleId::APP_MQTT,
        middleware::ModuleId::APP_MEDIA);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    const bool ok = middleware::MsgPort::instance().publish_async(msg);
    if(!ok) {
        UPC_LOG_MQTT_SERVICE(Warn, "dispatch stop_rtsp_push failed");
    } else {
        UPC_LOG_MQTT_SERVICE(Info,
            "dispatch stop_rtsp_push req_id=%s stream_type=%s",
            cmd.req_id.c_str(), cmd.stream_type.c_str());
    }
    return ok;
}

bool MqttService::track_pending_ack(const std::string& req_id, const std::string& cmd)
{
    if(req_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(pending_mtx_);
    if(pending_acks_.find(req_id) != pending_acks_.end()) {
        UPC_LOG_MQTT_SERVICE(Warn, "duplicate pending req_id=%s", req_id.c_str());
        return false;
    }
    PendingAck pending{};
    pending.cmd = cmd;
    pending.deadline_ms = monotonic_ms() + kRtspAckTimeoutMs;
    pending_acks_[req_id] = pending;
    return true;
}

void MqttService::expire_pending_acks()
{
    const uint64_t now = monotonic_ms();
    std::vector<std::pair<std::string, std::string>> expired;
    {
        std::lock_guard<std::mutex> lock(pending_mtx_);
        for(auto it = pending_acks_.begin(); it != pending_acks_.end();) {
            if(now >= it->second.deadline_ms) {
                expired.emplace_back(it->first, it->second.cmd);
                it = pending_acks_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for(const auto& item : expired) {
        UPC_LOG_MQTT_SERVICE(Warn,
            "rtsp ack timeout req_id=%s cmd=%s",
            item.first.c_str(), item.second.c_str());
        (void)publish_cmd_ack(item.first, item.second, "error",
                              kCmdRejectReasonEncoderError);
    }
}

void MqttService::on_rtsp_start_rsp(const middleware::Message& msg)
{
    handle_rtsp_rsp(msg, kCmdStartRtspPush);
}

void MqttService::on_rtsp_stop_rsp(const middleware::Message& msg)
{
    handle_rtsp_rsp(msg, kCmdStopRtspPush);
}

void MqttService::handle_rtsp_rsp(const middleware::Message& msg,
                                  const char* expect_cmd)
{
    middleware::RtspPushRspPayload rsp{};
    if(!read_inline_pod(msg.payload, rsp)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid rtsp rsp payload cmd=%s", expect_cmd);
        return;
    }
    rsp.req_id[sizeof(rsp.req_id) - 1] = '\0';
    rsp.reason[sizeof(rsp.reason) - 1] = '\0';
    rsp.stream_type[sizeof(rsp.stream_type) - 1] = '\0';

    {
        std::lock_guard<std::mutex> lock(pending_mtx_);
        auto it = pending_acks_.find(rsp.req_id);
        if(it == pending_acks_.end()) {
            UPC_LOG_MQTT_SERVICE(Warn,
                "rtsp rsp without pending req_id=%s", rsp.req_id);
            return;
        }
        if(it->second.cmd != expect_cmd) {
            UPC_LOG_MQTT_SERVICE(Warn,
                "rtsp rsp cmd mismatch req_id=%s expect=%s got=%s",
                rsp.req_id, expect_cmd, it->second.cmd.c_str());
            return;
        }
        pending_acks_.erase(it);
    }

    const char* result = "error";
    const char* reason = nullptr;
    char detail[128];
    std::snprintf(detail, sizeof(detail), "{}");
    switch(static_cast<middleware::RtspPushResult>(rsp.result)) {
    case middleware::RtspPushResult::Accepted:
        result = "accepted";
        if(rsp.push_started_at > 0) {
            std::snprintf(detail, sizeof(detail),
                          "{\"push_started_at\":%llu}",
                          static_cast<unsigned long long>(rsp.push_started_at));
        }
        break;
    case middleware::RtspPushResult::Rejected:
        result = "rejected";
        reason = rsp.reason[0] ? rsp.reason : kCmdRejectReasonInvalidState;
        break;
    case middleware::RtspPushResult::Error:
    default:
        result = "error";
        reason = rsp.reason[0] ? rsp.reason : kCmdRejectReasonEncoderError;
        break;
    }
    (void)publish_cmd_ack(rsp.req_id, expect_cmd, result, reason, detail);
}

void MqttService::handle_ota_notify(const std::string& payload)
{
    OtaNotify notify{};
    if(!parse_ota_notify(payload, notify)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid ota notify payload");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(ota_mtx_);
        pending_ota_notify_ = notify;
    }
    middleware::OtaNotifyPayload ntf{};
    std::snprintf(ntf.task_id, sizeof(ntf.task_id), "%s", notify.task_id.c_str());
    std::snprintf(ntf.release_version, sizeof(ntf.release_version), "%s",
                  notify.release_version.c_str());
    ntf.required = notify.required ? uint8_t(1) : uint8_t(0);
    ntf.approval_status = static_cast<uint8_t>(notify.approval_status);
    middleware::Message msg(middleware::MsgType::MQTT_Ota_Notify_Ntf,
                            middleware::ModuleId::APP_MQTT,
                            middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&ntf), sizeof(ntf));
    (void)middleware::MsgPort::instance().publish_async(msg);
}

void MqttService::handle_ota_manifest_response(const std::string& payload)
{
    std::string task_id;
    std::string err_code;
    std::vector<OtaComponent> components;
    if(!parse_manifest_response(payload, task_id, components, err_code)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid ota manifest response");
        return;
    }
    if(task_id.empty()) {
        UPC_LOG_MQTT_SERVICE(Warn, "ota manifest response without task_id");
        return;
    }
    if(!err_code.empty()) {
        middleware::OtaProgressPayload p{};
        std::snprintf(p.task_id, sizeof(p.task_id), "%s", task_id.c_str());
        p.state = static_cast<uint8_t>(middleware::OtaProgressState::Failed);
        p.progress_pct = 0;
        std::snprintf(p.error_code, sizeof(p.error_code), "%s", err_code.c_str());
        publish_ota_progress_ui(p);
        (void)publish_ota_progress_cloud(p);
        return;
    }
    start_manifest_download(std::move(task_id), std::move(components));
}

bool MqttService::publish_manifest_request(const std::string& task_id)
{
    middleware::McuVersionPayload mcu{};
    middleware::GalvoVersionPayload galvo{};
    bool mcu_valid = false;
    bool galvo_valid = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        mcu = fw_cache_.mcu;
        galvo = fw_cache_.galvo;
        mcu_valid = fw_cache_.mcu_valid;
        galvo_valid = fw_cache_.galvo_valid;
    }
    const std::string mcu_version = mcu_valid ? format_mcu_version(mcu) : "unknown";
    const std::string galvo_version = galvo_valid ? format_galvo_version(galvo.fw_version_le)
                                                  : "unknown";
    const std::string req_id = std::string("manifest-") + task_id;
    const uint32_t seq = ++seq_;
    const long long ts = static_cast<long long>(unix_ms());
    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"ota_manifest_request\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"req_id\":\"%s\","
        "\"data\":{"
            "\"task_id\":\"%s\","
            "\"versions\":{"
                "\"soc\":\"%s\","
                "\"mcu\":\"%s\","
                "\"glavo\":\"%s\""
            "}"
        "}"
        "}",
        json_escape(cfg_.device_id).c_str(),
        ts,
        seq,
        json_escape(req_id).c_str(),
        json_escape(task_id).c_str(),
        json_escape(UPC_SW_VERSION).c_str(),
        json_escape(mcu_version).c_str(),
        json_escape(galvo_version).c_str());
    const std::string topic = cfg_.topic_ota_manifest_req();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
    msg.qos = cfg_.qos_ack;
    msg.retained = 0;
    MQTTClient_deliveryToken token{};
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish ota manifest request failed rc=%d", rc);
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        ota_subscribed_ = false;
        return false;
    }
    UPC_LOG_MQTT_SERVICE(Info, "ota manifest request sent task_id=%s token=%d",
                         task_id.c_str(), static_cast<int>(token));
    return true;
}

bool MqttService::publish_ota_progress_cloud(const middleware::OtaProgressPayload& progress)
{
    const char* state = "downloading";
    switch(static_cast<middleware::OtaProgressState>(progress.state)) {
    case middleware::OtaProgressState::Verifying: state = "verifying"; break;
    case middleware::OtaProgressState::Applying: state = "applying"; break;
    case middleware::OtaProgressState::Completed: state = "completed"; break;
    case middleware::OtaProgressState::Failed: state = "failed"; break;
    case middleware::OtaProgressState::RolledBack: state = "rolled_back"; break;
    case middleware::OtaProgressState::Downloading:
    default: break;
    }
    char payload[1024];
    std::snprintf(payload, sizeof(payload),
        "{"
        "\"v\":1,"
        "\"type\":\"ota_progress\","
        "\"device_id\":\"%s\","
        "\"ts\":%lld,"
        "\"seq\":%u,"
        "\"data\":{"
            "\"task_id\":\"%s\","
            "\"component\":\"%s\","
            "\"state\":\"%s\","
            "\"progress_pct\":%u,"
            "\"error_code\":\"%s\""
        "}"
        "}",
        json_escape(cfg_.device_id).c_str(),
        static_cast<long long>(unix_ms()),
        ++seq_,
        json_escape(progress.task_id).c_str(),
        json_escape(progress.component).c_str(),
        state,
        static_cast<unsigned>(progress.progress_pct),
        json_escape(progress.error_code).c_str());
    const std::string topic = cfg_.topic_ota_progress();
    MQTTClient_message msg = MQTTClient_message_initializer;
    msg.payload = payload;
    msg.payloadlen = static_cast<int>(std::strlen(payload));
    msg.qos = cfg_.qos_ack;
    msg.retained = 0;
    MQTTClient_deliveryToken token{};
    const int rc = MQTTClient_publishMessage(client_, topic.c_str(), &msg, &token);
    if(rc != MQTTCLIENT_SUCCESS) {
        UPC_LOG_MQTT_SERVICE(Warn, "publish ota progress failed rc=%d", rc);
        mqtt_connected_ = false;
        cmd_subscribed_ = false;
        ota_subscribed_ = false;
        return false;
    }
    return true;
}

void MqttService::publish_ota_progress_ui(const middleware::OtaProgressPayload& progress)
{
    middleware::Message msg(middleware::MsgType::APP_Mqtt_Ota_Progress_Ntf,
                            middleware::ModuleId::APP_MQTT,
                            middleware::ModuleId::UI);
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&progress), sizeof(progress));
    (void)middleware::MsgPort::instance().publish_async(msg);
}

void MqttService::start_manifest_download(std::string task_id,
                                          std::vector<OtaComponent> components)
{
    {
        std::lock_guard<std::mutex> lock(ota_mtx_);
        if(ota_download_running_) {
            UPC_LOG_MQTT_SERVICE(Warn, "ota download already running, ignore task_id=%s",
                                 task_id.c_str());
            return;
        }
        ota_download_running_ = true;
    }
    if(ota_download_thread_.joinable()) {
        ota_download_thread_.join();
    }
    ota_download_thread_ = std::thread(
        [this, task_id = std::move(task_id), components = std::move(components)]() mutable {
            run_manifest_download_task(std::move(task_id), std::move(components));
            std::lock_guard<std::mutex> lock(ota_mtx_);
            ota_download_running_ = false;
        });
}

void MqttService::run_manifest_download_task(std::string task_id,
                                             std::vector<OtaComponent> components)
{
    auto publish_failed = [this, &task_id](const char* component, const char* code) {
        middleware::OtaProgressPayload p{};
        std::snprintf(p.task_id, sizeof(p.task_id), "%s", task_id.c_str());
        std::snprintf(p.component, sizeof(p.component), "%s", component ? component : "");
        p.state = static_cast<uint8_t>(middleware::OtaProgressState::Failed);
        p.progress_pct = 0;
        std::snprintf(p.error_code, sizeof(p.error_code), "%s", code ? code : "unknown");
        publish_ota_progress_ui(p);
        (void)publish_ota_progress_cloud(p);
    };

    if(components.empty()) {
        publish_failed("all", "empty_manifest");
        return;
    }

    if(!ensure_dir(kImageTmpDir)) {
        publish_failed("all", "tmp_dir_failed");
        return;
    }

    bool handled_any_component = false;
    for(const OtaComponent& comp : components) {
        const char* target_dir = component_target_dir(comp.component);
        if(!target_dir) {
            publish_failed(comp.component.c_str(), "invalid_component");
            return;
        }
        handled_any_component = true;

        middleware::OtaProgressPayload progress{};
        std::snprintf(progress.task_id, sizeof(progress.task_id), "%s", task_id.c_str());
        std::snprintf(progress.component, sizeof(progress.component), "%s", comp.component.c_str());
        progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Downloading);
        progress.progress_pct = 0;
        publish_ota_progress_ui(progress);
        (void)publish_ota_progress_cloud(progress);

        const std::string base_name = comp.file_name.empty()
            ? basename_from_url_path(comp.url)
            : comp.file_name;
        const std::string upper_base = to_upper_ascii(base_name);
        const bool is_tar_gz =
            (upper_base.size() >= 7 &&
             upper_base.rfind(".TAR.GZ") == upper_base.size() - 7) ||
            (upper_base.size() >= 4 &&
             upper_base.rfind(".TGZ") == upper_base.size() - 4);
        if(!is_tar_gz) {
            publish_failed(comp.component.c_str(), "unsupported_package");
            return;
        }

        const std::filesystem::path tmp_part =
            std::filesystem::path(kImageTmpDir) /
            (task_id + "_" + comp.component + "_" + base_name + ".part");
        const std::filesystem::path tmp_final =
            std::filesystem::path(kImageTmpDir) /
            (task_id + "_" + comp.component + "_" + base_name);
        const std::filesystem::path extract_dir =
            std::filesystem::path(kImageTmpDir) /
            (task_id + "_" + comp.component + "_extract");

        std::error_code ec;
        std::filesystem::remove(tmp_part, ec);
        std::filesystem::remove(tmp_final, ec);
        std::filesystem::remove_all(extract_dir, ec);

        std::string dl_err;
        const bool dl_ok = download_file_with_progress(
            comp.url, tmp_part,
            [this, &progress](size_t recv, size_t total) {
                if(total > 0) {
                    const int pct = static_cast<int>((recv * 100) / total);
                    progress.progress_pct = static_cast<uint8_t>(std::clamp(pct, 0, 80));
                } else if(progress.progress_pct < 80) {
                    progress.progress_pct = static_cast<uint8_t>(
                        std::min<int>(80, static_cast<int>(progress.progress_pct) + 1));
                }
                publish_ota_progress_ui(progress);
                (void)publish_ota_progress_cloud(progress);
            }, dl_err);
        if(!dl_ok) {
            std::filesystem::remove(tmp_part, ec);
            publish_failed(comp.component.c_str(), dl_err.c_str());
            return;
        }

        std::filesystem::rename(tmp_part, tmp_final, ec);
        if(ec) {
            std::filesystem::remove(tmp_part, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "tmp_rename_failed");
            return;
        }

        progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Verifying);
        progress.progress_pct = 90;
        publish_ota_progress_ui(progress);
        (void)publish_ota_progress_cloud(progress);

        const std::string local_sha = hex_sha256_file(tmp_final);
        std::string expected_sha = comp.sha256;
        std::transform(expected_sha.begin(), expected_sha.end(), expected_sha.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if(!expected_sha.empty() && local_sha != expected_sha) {
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "sha256_mismatch");
            return;
        }

        if(!ensure_dir(extract_dir)) {
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "extract_dir_failed");
            return;
        }

        std::vector<std::string> tar_entries;
        const std::string list_cmd =
            "tar -tzf " + sh_single_quote(tmp_final.string());
        if(!run_capture_lines(list_cmd, tar_entries)) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "tar_list_failed");
            return;
        }
        bool has_app1 = false;
        bool has_app2 = false;
        for(const std::string& entry_raw : tar_entries) {
            const std::string entry = trim_line(entry_raw);
            if(entry.empty()) {
                continue;
            }
            if(!archive_entry_safe_flat(entry) || !is_bin_name(entry)) {
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "tar_entry_invalid");
                return;
            }
            const std::string upper = to_upper_ascii(entry);
            if(upper.find("APP1") != std::string::npos) {
                has_app1 = true;
            }
            if(upper.find("APP2") != std::string::npos) {
                has_app2 = true;
            }
        }
        if(!has_app1 || !has_app2) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "tar_missing_ab");
            return;
        }

        const std::string extract_cmd =
            "tar -xzf " + sh_single_quote(tmp_final.string()) +
            " -C " + sh_single_quote(extract_dir.string());
        if(!run_shell_ok(extract_cmd)) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "tar_extract_failed");
            return;
        }

        bool extracted_app1 = false;
        bool extracted_app2 = false;
        std::vector<std::filesystem::path> extracted_files;
        for(const auto& ent : std::filesystem::directory_iterator(extract_dir, ec)) {
            if(ec || !ent.is_regular_file()) {
                continue;
            }
            const std::string name = ent.path().filename().string();
            if(!is_bin_name(name)) {
                continue;
            }
            extracted_files.push_back(ent.path());
            const std::string upper = to_upper_ascii(name);
            if(upper.find("APP1") != std::string::npos) {
                extracted_app1 = true;
            }
            if(upper.find("APP2") != std::string::npos) {
                extracted_app2 = true;
            }
        }
        if(!extracted_app1 || !extracted_app2 || extracted_files.size() != 2) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "extract_ab_invalid");
            return;
        }

        progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Applying);
        progress.progress_pct = 100;
        publish_ota_progress_ui(progress);
        (void)publish_ota_progress_cloud(progress);

        if(!ensure_dir(target_dir)) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "target_dir_failed");
            return;
        }

        const std::filesystem::path backup_dir =
            std::filesystem::path(kImageTmpDir) /
            (task_id + "_" + comp.component + "_backup");
        std::filesystem::remove_all(backup_dir, ec);
        if(!ensure_dir(backup_dir)) {
            std::filesystem::remove_all(extract_dir, ec);
            std::filesystem::remove(tmp_final, ec);
            publish_failed(comp.component.c_str(), "backup_dir_failed");
            return;
        }

        std::vector<std::filesystem::path> backup_paths;
        for(const auto& ent : std::filesystem::directory_iterator(target_dir, ec)) {
            if(ec || !ent.is_regular_file()) {
                continue;
            }
            const std::filesystem::path dst = backup_dir / ent.path().filename();
            std::filesystem::rename(ent.path(), dst, ec);
            if(ec) {
                for(const auto& back : backup_paths) {
                    const std::filesystem::path restore = std::filesystem::path(target_dir) / back.filename();
                    std::filesystem::rename(back, restore, ec);
                }
                std::filesystem::remove_all(backup_dir, ec);
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "backup_old_failed");
                return;
            }
            backup_paths.push_back(dst);
        }

        std::vector<std::filesystem::path> moved_new_files;
        for(const auto& src : extracted_files) {
            const auto dst = std::filesystem::path(target_dir) / src.filename();
            std::filesystem::rename(src, dst, ec);
            if(ec) {
                for(const auto& moved : moved_new_files) {
                    std::filesystem::remove(moved, ec);
                }
                for(const auto& back : backup_paths) {
                    const std::filesystem::path restore = std::filesystem::path(target_dir) / back.filename();
                    std::filesystem::rename(back, restore, ec);
                }
                std::filesystem::remove_all(backup_dir, ec);
                std::filesystem::remove_all(extract_dir, ec);
                std::filesystem::remove(tmp_final, ec);
                publish_failed(comp.component.c_str(), "move_failed");
                return;
            }
            moved_new_files.push_back(dst);
        }

        std::filesystem::remove_all(backup_dir, ec);
        std::filesystem::remove_all(extract_dir, ec);
        std::filesystem::remove(tmp_final, ec);

        progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Completed);
        progress.progress_pct = 100;
        progress.error_code[0] = '\0';
        publish_ota_progress_ui(progress);
        (void)publish_ota_progress_cloud(progress);

        if(!comp.version.empty()) {
            std::lock_guard<std::mutex> lock(data_mtx_);
            if(comp.component == "soc" && env_sim_on_) {
                soc_fw_override_ = comp.version;
            } else if(comp.component == "mcu" && env_sim_on_) {
                middleware::McuVersionPayload parsed{};
                if(parse_mcu_version_text(comp.version, parsed)) {
                    fw_cache_.mcu = parsed;
                    fw_cache_.mcu_valid = true;
                }
            } else if(comp.component == "glavo" && env_sim_on_) {
                middleware::GalvoVersionPayload parsed{};
                if(parse_galvo_version_text(comp.version, parsed)) {
                    fw_cache_.galvo = parsed;
                    fw_cache_.galvo_valid = true;
                }
            }
        }
    }

    if(!handled_any_component) {
        publish_failed("all", "no_supported_component");
        return;
    }

    bool need_mcu_refresh = false;
    bool need_galvo_refresh = false;
    bool env_sim_on = false;
    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        env_sim_on = env_sim_on_;
    }
    for(const OtaComponent& comp : components) {
        if(comp.component == "mcu") {
            need_mcu_refresh = true;
        } else if(comp.component == "glavo") {
            need_galvo_refresh = true;
        }
    }

    if(env_sim_on) {
        for(const OtaComponent& comp : components) {
            if(comp.component == "mcu" && !comp.version.empty()) {
                need_mcu_refresh = false;
            } else if(comp.component == "glavo" && !comp.version.empty()) {
                need_galvo_refresh = false;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(data_mtx_);
        ota_hb_wait_.active = (need_mcu_refresh || need_galvo_refresh);
        ota_hb_wait_.need_mcu = need_mcu_refresh;
        ota_hb_wait_.need_galvo = need_galvo_refresh;
        ota_hb_wait_.got_mcu = false;
        ota_hb_wait_.got_galvo = false;
        ota_hb_wait_.deadline_ms = monotonic_ms() + 3000;
    }

    if(need_mcu_refresh || need_galvo_refresh) {
        request_firmware_versions_force(need_mcu_refresh, need_galvo_refresh);
    }

    if(!need_mcu_refresh && !need_galvo_refresh) {
        last_heartbeat_pub_ms_ = 0;
        (void)publish_heartbeat();
    }
}

bool MqttService::parse_ota_notify(const std::string& payload, OtaNotify& out) const
{
    out = OtaNotify{};
    std::string data;
    if(!json_extract_object(payload, "data", data)) {
        return false;
    }
    if(!json_extract_string(data, "task_id", out.task_id)) {
        (void)json_extract_string(data, "taskId", out.task_id);
    }
    if(out.task_id.empty()) {
        return false;
    }
    (void)json_extract_string(data, "release_version", out.release_version);
    if(out.release_version.empty()) {
        (void)json_extract_string(data, "releaseVersion", out.release_version);
    }
    bool required = false;
    if(json_extract_bool(data, "required", required)) {
        out.required = required;
    }
    std::string approval;
    if(json_extract_string(data, "approval_status", approval) ||
       json_extract_string(data, "approvalStatus", approval)) {
        (void)map_approval_status(approval, out.approval_status);
    }
    return true;
}

bool MqttService::parse_manifest_response(const std::string& payload,
                                          std::string& task_id,
                                          std::vector<OtaComponent>& components,
                                          std::string& err_code) const
{
    task_id.clear();
    components.clear();
    err_code.clear();
    std::string data;
    if(!json_extract_object(payload, "data", data)) {
        return false;
    }
    (void)json_extract_string(data, "task_id", task_id);
    if(task_id.empty()) {
        (void)json_extract_string(data, "taskId", task_id);
    }
    (void)json_extract_string(data, "error_code", err_code);
    if(err_code.empty()) {
        (void)json_extract_string(data, "errorCode", err_code);
    }

    std::vector<std::string> objs;
    if(!json_extract_array_objects(data, "components", objs)) {
        return !task_id.empty();
    }
    for(const std::string& obj : objs) {
        OtaComponent c{};
        if(!json_extract_string(obj, "component", c.component) || c.component.empty()) {
            continue;
        }
        (void)json_extract_string(obj, "version", c.version);
        (void)json_extract_string(obj, "url", c.url);
        (void)json_extract_string(obj, "sha256", c.sha256);
        (void)json_extract_string(obj, "file_name", c.file_name);
        if(c.file_name.empty()) {
            (void)json_extract_string(obj, "filename", c.file_name);
        }
        int size_bytes = 0;
        if(json_extract_int(obj, "size_bytes", size_bytes) && size_bytes > 0) {
            c.size_bytes = static_cast<uint64_t>(size_bytes);
        }
        if(c.url.empty()) {
            continue;
        }
        if(c.file_name.empty()) {
            ParsedUrl parsed{};
            if(parse_url(c.url, parsed)) {
                c.file_name = basename_from_url_path(parsed.path);
            }
        }
        components.push_back(std::move(c));
    }
    return !task_id.empty();
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
    out = std::atoi(json.substr(pos, end - pos).c_str());
    return true;
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
