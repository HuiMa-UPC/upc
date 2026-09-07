#include "mqtt_service.hpp"

#include "mqtt_service_internal.hpp"
#include "rs_log.hpp"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

namespace upc::mqtt {

namespace {

bool topic_file_name_safe(const std::string& name)
{
    if(name.empty() || name.size() > 128) {
        return false;
    }
    if(name.find('/') != std::string::npos ||
       name.find('\\') != std::string::npos ||
       name.find("..") != std::string::npos) {
        return false;
    }
    /* 字典文件名：topic_*.log（逐字节对齐云端白名单）。 */
    if(name.rfind("topic_", 0) != 0) {
        return false;
    }
    if(name.size() < 10 || name.compare(name.size() - 4, 4, ".log") != 0) {
        return false;
    }
    for(unsigned char c : name) {
        if(!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

bool base64_decode(const std::string& in, std::vector<unsigned char>& out)
{
    out.clear();
    if(in.empty()) {
        return false;
    }
    out.resize((in.size() / 4) * 3 + 3);
    const int n = EVP_DecodeBlock(out.data(),
                                  reinterpret_cast<const unsigned char*>(in.data()),
                                  static_cast<int>(in.size()));
    if(n < 0) {
        out.clear();
        return false;
    }
    size_t pad = 0;
    if(!in.empty() && in.back() == '=') {
        ++pad;
    }
    if(in.size() >= 2 && in[in.size() - 2] == '=') {
        ++pad;
    }
    if(static_cast<size_t>(n) < pad) {
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(n) - pad);
    return !out.empty();
}

bool hkdf_sha256_aes_key(const std::string& ikm,
                         const std::vector<unsigned char>& salt,
                         unsigned char key_out[kLogAesKeyLen])
{
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if(!pctx) {
        return false;
    }
    bool ok = false;
    do {
        if(EVP_PKEY_derive_init(pctx) <= 0) {
            break;
        }
        if(EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) <= 0) {
            break;
        }
        if(EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt.data(),
                                       static_cast<int>(salt.size())) <= 0) {
            break;
        }
        if(EVP_PKEY_CTX_set1_hkdf_key(
               pctx,
               reinterpret_cast<const unsigned char*>(ikm.data()),
               static_cast<int>(ikm.size())) <= 0) {
            break;
        }
        if(EVP_PKEY_CTX_add1_hkdf_info(
               pctx,
               reinterpret_cast<const unsigned char*>(kLogHkdfInfo),
               static_cast<int>(std::strlen(kLogHkdfInfo))) <= 0) {
            break;
        }
        size_t key_len = kLogAesKeyLen;
        if(EVP_PKEY_derive(pctx, key_out, &key_len) <= 0 || key_len != kLogAesKeyLen) {
            break;
        }
        ok = true;
    } while(false);
    EVP_PKEY_CTX_free(pctx);
    return ok;
}

/**
 * AES-256-GCM：输出 nonce(12) ‖ ciphertext ‖ tag(16)。
 * AAD = "v1:" + collectId。
 */
bool aes_256_gcm_seal(const unsigned char key[kLogAesKeyLen],
                      const std::string& aad,
                      const std::vector<unsigned char>& plaintext,
                      std::vector<unsigned char>& sealed)
{
    sealed.clear();
    unsigned char nonce[kLogGcmNonceLen]{};
    if(RAND_bytes(nonce, static_cast<int>(kLogGcmNonceLen)) != 1) {
        return false;
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if(!ctx) {
        return false;
    }

    std::vector<unsigned char> ciphertext(plaintext.size() + EVP_MAX_BLOCK_LENGTH);
    unsigned char tag[kLogGcmTagLen]{};
    int len = 0;
    int ciphertext_len = 0;
    bool ok = false;

    do {
        if(EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1) {
            break;
        }
        if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                               static_cast<int>(kLogGcmNonceLen), nullptr) != 1) {
            break;
        }
        if(EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) {
            break;
        }
        if(!aad.empty()) {
            if(EVP_EncryptUpdate(ctx, nullptr, &len,
                                 reinterpret_cast<const unsigned char*>(aad.data()),
                                 static_cast<int>(aad.size())) != 1) {
                break;
            }
        }
        if(!plaintext.empty()) {
            if(EVP_EncryptUpdate(ctx, ciphertext.data(), &len,
                                 plaintext.data(),
                                 static_cast<int>(plaintext.size())) != 1) {
                break;
            }
            ciphertext_len = len;
        }
        if(EVP_EncryptFinal_ex(ctx, ciphertext.data() + ciphertext_len, &len) != 1) {
            break;
        }
        ciphertext_len += len;
        if(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                               static_cast<int>(kLogGcmTagLen), tag) != 1) {
            break;
        }
        ok = true;
    } while(false);

    EVP_CIPHER_CTX_free(ctx);
    if(!ok) {
        return false;
    }

    sealed.resize(kLogGcmNonceLen + static_cast<size_t>(ciphertext_len) + kLogGcmTagLen);
    std::memcpy(sealed.data(), nonce, kLogGcmNonceLen);
    if(ciphertext_len > 0) {
        std::memcpy(sealed.data() + kLogGcmNonceLen, ciphertext.data(),
                    static_cast<size_t>(ciphertext_len));
    }
    std::memcpy(sealed.data() + kLogGcmNonceLen + static_cast<size_t>(ciphertext_len),
                tag, kLogGcmTagLen);
    return true;
}

bool read_file_bytes(const std::filesystem::path& path, std::vector<unsigned char>& out)
{
    out.clear();
    std::ifstream in(path, std::ios::binary);
    if(!in) {
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff sz = in.tellg();
    if(sz < 0) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if(sz > 0 && !in.read(reinterpret_cast<char*>(out.data()), sz)) {
        out.clear();
        return false;
    }
    return true;
}

int64_t regular_file_size(const std::filesystem::path& path)
{
    std::error_code ec;
    if(!std::filesystem::is_regular_file(path, ec) || ec) {
        return -1;
    }
    const auto sz = std::filesystem::file_size(path, ec);
    if(ec) {
        return -1;
    }
    return static_cast<int64_t>(sz);
}

/**
 * 收集逻辑名 F 对应的物理文件：F、F.N、F.N.gz（N=1..rotate）。
 * 有什么收什么；轮转未触发时可以只有明文 .log。
 */
void discover_physical_files(const std::string& logical_name,
                             std::vector<std::filesystem::path>& paths,
                             int64_t& total_size)
{
    paths.clear();
    total_size = 0;
    const std::filesystem::path dir(kTopicLogDir);

    auto try_add = [&](const std::string& name) {
        const std::filesystem::path p = dir / name;
        const int64_t sz = regular_file_size(p);
        if(sz < 0) {
            return;
        }
        paths.push_back(p);
        total_size += sz;
    };

    try_add(logical_name);
    for(int gen = 1; gen <= kLogRotateMaxGenerations; ++gen) {
        try_add(logical_name + "." + std::to_string(gen));
        try_add(logical_name + "." + std::to_string(gen) + ".gz");
    }
}

bool create_tar_gz(const std::vector<std::filesystem::path>& files,
                   const std::filesystem::path& out_tar,
                   std::string& err_reason)
{
    if(files.empty()) {
        err_reason = kLogErrInternal;
        return false;
    }
    std::ostringstream cmd;
    cmd << "tar -czf " << sh_single_quote(out_tar.string()) << " -C "
        << sh_single_quote(kTopicLogDir);
    for(const auto& p : files) {
        cmd << ' ' << sh_single_quote(p.filename().string());
    }
    if(!run_shell_ok(cmd.str())) {
        /* tar 失败常见原因：磁盘满或无 tar 二进制。 */
        std::error_code ec;
        const auto space = std::filesystem::space(out_tar.parent_path(), ec);
        if(!ec && space.available < 64 * 1024) {
            err_reason = kLogErrDiskFull;
        } else {
            err_reason = kLogErrInternal;
        }
        return false;
    }
    return true;
}

std::string json_escape_local(const std::string& text)
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

std::string build_file_report_json(const std::vector<MqttService::LogFileReport>& reports)
{
    std::ostringstream oss;
    oss << '[';
    for(size_t i = 0; i < reports.size(); ++i) {
        const auto& r = reports[i];
        if(i > 0) {
            oss << ',';
        }
        oss << "{\"file\":\"" << json_escape_local(r.file) << "\","
            << "\"collected\":" << (r.collected ? "true" : "false");
        if(r.collected) {
            oss << ",\"size\":" << r.size;
        } else if(r.reason == kLogFileEmpty) {
            oss << ",\"size\":0,\"reason\":\"" << r.reason << '"';
        } else if(r.reason == kLogFileSizeExceeded) {
            oss << ",\"size\":" << r.size << ",\"reason\":\"" << r.reason << '"';
        } else {
            oss << ",\"reason\":\"" << json_escape_local(r.reason) << '"';
        }
        oss << '}';
    }
    oss << ']';
    return oss.str();
}

} // namespace

bool MqttService::parse_log_collect_command(const std::string& data_json,
                                            const std::string& req_id,
                                            LogCollectCommand& out,
                                            std::string& reject_reason) const
{
    reject_reason.clear();
    out = LogCollectCommand{};
    out.req_id = req_id;

    if(!json_extract_string(data_json, "collectId", out.collect_id) ||
       out.collect_id.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_string(data_json, "objectKey", out.object_key) ||
       out.object_key.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_string(data_json, "uploadUrl", out.upload_url) ||
       out.upload_url.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_int64(data_json, "uploadUrlExpiresAt", out.upload_url_expires_at) ||
       out.upload_url_expires_at <= 0) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_int64(data_json, "maxPacketSize", out.max_packet_size) ||
       out.max_packet_size <= static_cast<int64_t>(kLogGcmNonceLen + kLogGcmTagLen)) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_string_array(data_json, "topicFiles", out.topic_files) ||
       out.topic_files.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    for(const auto& name : out.topic_files) {
        if(!topic_file_name_safe(name)) {
            reject_reason = kCmdRejectReasonInvalidState;
            return false;
        }
    }

    std::string encrypt_json;
    if(!json_extract_object(data_json, "encrypt", encrypt_json)) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    std::string algorithm;
    if(!json_extract_string(encrypt_json, "algorithm", algorithm) ||
       algorithm != kLogEncryptAlgorithm) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    if(!json_extract_string(encrypt_json, "salt", out.encrypt_salt_b64) ||
       out.encrypt_salt_b64.empty()) {
        reject_reason = kCmdRejectReasonInvalidState;
        return false;
    }
    return true;
}

bool MqttService::start_log_collect(LogCollectCommand cmd)
{
    {
        std::lock_guard<std::mutex> lock(log_collect_mtx_);
        if(log_collect_running_) {
            UPC_LOG_MQTT_SERVICE(Warn,
                "log_collect already running, reject collectId=%s",
                cmd.collect_id.c_str());
            return false;
        }
        log_collect_running_ = true;
    }
    if(log_collect_thread_.joinable()) {
        log_collect_thread_.join();
    }
    log_collect_thread_ = std::thread([this, cmd = std::move(cmd)]() mutable {
        run_log_collect_task(std::move(cmd));
        std::lock_guard<std::mutex> lock(log_collect_mtx_);
        log_collect_running_ = false;
    });
    return true;
}

void MqttService::publish_log_collect_error(const LogCollectCommand& cmd,
                                            const char* reason)
{
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "{\"collectId\":\"%s\"}",
                  json_escape(cmd.collect_id).c_str());
    (void)publish_cmd_ack(cmd.req_id, kCmdLogCollect, "error", reason, detail);
}

void MqttService::publish_log_collect_accepted(const LogCollectCommand& cmd,
                                               int64_t file_size,
                                               const std::vector<LogFileReport>& reports)
{
    /* encryptParams 是 JSON 字符串字段：salt 用下行 base64 原文，不 decode 重编。 */
    const std::string encrypt_params_inner =
        std::string("{\"algorithm\":\"") + kLogEncryptAlgorithm +
        "\",\"salt\":\"" + json_escape(cmd.encrypt_salt_b64) + "\"}";
    const std::string file_report = build_file_report_json(reports);

    std::ostringstream detail;
    detail << '{'
           << "\"collectId\":\"" << json_escape(cmd.collect_id) << "\","
           << "\"objectKey\":\"" << json_escape(cmd.object_key) << "\","
           << "\"checksum\":\"\","
           << "\"fileSize\":" << file_size << ','
           << "\"encryptParams\":\"" << json_escape(encrypt_params_inner) << "\","
           << "\"fileReport\":" << file_report
           << '}';
    (void)publish_cmd_ack(cmd.req_id, kCmdLogCollect, "accepted", nullptr,
                          detail.str().c_str());
}

void MqttService::run_log_collect_task(LogCollectCommand cmd)
{
    UPC_LOG_MQTT_SERVICE(Info,
        "log_collect start collectId=%s files=%zu maxPacket=%lld",
        cmd.collect_id.c_str(),
        cmd.topic_files.size(),
        static_cast<long long>(cmd.max_packet_size));

    const int64_t now_s = unix_ms() / 1000;
    if(now_s >= cmd.upload_url_expires_at) {
        publish_log_collect_error(cmd, kLogErrUrlExpired);
        return;
    }

    if(cfg_.device_secret.empty()) {
        UPC_LOG_MQTT_SERVICE(Error, "log_collect missing device_secret");
        publish_log_collect_error(cmd, kLogErrEncryptFail);
        return;
    }

    std::vector<unsigned char> salt_bin;
    if(!base64_decode(cmd.encrypt_salt_b64, salt_bin)) {
        publish_log_collect_error(cmd, kLogErrEncryptFail);
        return;
    }

    unsigned char aes_key[kLogAesKeyLen]{};
    if(!hkdf_sha256_aes_key(cfg_.device_secret, salt_bin, aes_key)) {
        publish_log_collect_error(cmd, kLogErrEncryptFail);
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(kLogCollectTmpDir, ec);
    if(ec) {
        publish_log_collect_error(cmd, kLogErrDiskFull);
        return;
    }

    const std::filesystem::path work_dir =
        std::filesystem::path(kLogCollectTmpDir) / cmd.collect_id;
    std::filesystem::remove_all(work_dir, ec);
    if(!std::filesystem::create_directories(work_dir, ec)) {
        publish_log_collect_error(cmd, kLogErrDiskFull);
        return;
    }

    struct LogicalGroup {
        std::string name;
        std::vector<std::filesystem::path> paths;
        int64_t total_size = 0;
        LogFileReport report;
    };

    std::vector<LogicalGroup> groups;
    groups.reserve(cmd.topic_files.size());
    for(const auto& name : cmd.topic_files) {
        LogicalGroup g;
        g.name = name;
        g.report.file = name;
        discover_physical_files(name, g.paths, g.total_size);
        if(g.paths.empty()) {
            g.report.collected = false;
            g.report.reason = kLogFileNotFound;
        } else if(g.total_size <= 0) {
            g.report.collected = false;
            g.report.size = 0;
            g.report.reason = kLogFileEmpty;
            g.paths.clear();
        } else {
            g.report.collected = true;
            g.report.size = g.total_size;
        }
        groups.push_back(std::move(g));
    }

    /* 按 topicFiles 顺序贪心纳入；超密文上限则整组跳过 size_exceeded。 */
    std::vector<std::filesystem::path> selected;
    std::vector<LogFileReport> reports;
    reports.reserve(groups.size());
    bool any_size_exceeded = false;

    for(auto& g : groups) {
        if(!g.report.collected) {
            reports.push_back(g.report);
            continue;
        }

        std::vector<std::filesystem::path> trial = selected;
        trial.insert(trial.end(), g.paths.begin(), g.paths.end());

        const std::filesystem::path tar_path = work_dir / "packet.tar.gz";
        std::filesystem::remove(tar_path, ec);
        std::string pack_err;
        if(!create_tar_gz(trial, tar_path, pack_err)) {
            std::filesystem::remove_all(work_dir, ec);
            publish_log_collect_error(cmd, pack_err.c_str());
            return;
        }

        std::vector<unsigned char> tar_bytes;
        if(!read_file_bytes(tar_path, tar_bytes)) {
            std::filesystem::remove_all(work_dir, ec);
            publish_log_collect_error(cmd, kLogErrInternal);
            return;
        }

        const int64_t cipher_est =
            static_cast<int64_t>(kLogGcmNonceLen + tar_bytes.size() + kLogGcmTagLen);
        if(cipher_est > cmd.max_packet_size) {
            g.report.collected = false;
            g.report.reason = kLogFileSizeExceeded;
            g.report.size = g.total_size;
            any_size_exceeded = true;
            reports.push_back(g.report);
            continue;
        }

        selected = std::move(trial);
        reports.push_back(g.report);
    }

    const bool any_collected =
        std::any_of(reports.begin(), reports.end(),
                    [](const LogFileReport& r) { return r.collected; });
    if(!any_collected) {
        std::filesystem::remove_all(work_dir, ec);
        publish_log_collect_error(
            cmd, any_size_exceeded ? kLogErrInternal : kLogErrNoFileFound);
        return;
    }

    const std::filesystem::path final_tar = work_dir / "final.tar.gz";
    std::filesystem::remove(final_tar, ec);
    std::string pack_err;
    if(!create_tar_gz(selected, final_tar, pack_err)) {
        std::filesystem::remove_all(work_dir, ec);
        publish_log_collect_error(cmd, pack_err.c_str());
        return;
    }

    std::vector<unsigned char> tar_bytes;
    if(!read_file_bytes(final_tar, tar_bytes)) {
        std::filesystem::remove_all(work_dir, ec);
        publish_log_collect_error(cmd, kLogErrInternal);
        return;
    }

    const std::string aad = std::string("v1:") + cmd.collect_id;
    std::vector<unsigned char> sealed;
    if(!aes_256_gcm_seal(aes_key, aad, tar_bytes, sealed)) {
        std::filesystem::remove_all(work_dir, ec);
        publish_log_collect_error(cmd, kLogErrEncryptFail);
        return;
    }

    if(static_cast<int64_t>(sealed.size()) > cmd.max_packet_size) {
        std::filesystem::remove_all(work_dir, ec);
        publish_log_collect_error(cmd, kLogErrInternal);
        return;
    }

    if(unix_ms() / 1000 >= cmd.upload_url_expires_at) {
        std::filesystem::remove_all(work_dir, ec);
        publish_log_collect_error(cmd, kLogErrUrlExpired);
        return;
    }

    std::string put_err;
    if(!upload_http_put(cmd.upload_url, sealed.data(), sealed.size(), put_err)) {
        UPC_LOG_MQTT_SERVICE(Warn, "log_collect PUT failed collectId=%s err=%s",
                             cmd.collect_id.c_str(), put_err.c_str());
        std::filesystem::remove_all(work_dir, ec);
        const char* reason = kLogErrPutFailed;
        if(put_err.find("403") != std::string::npos ||
           put_err.find("401") != std::string::npos) {
            if(unix_ms() / 1000 >= cmd.upload_url_expires_at) {
                reason = kLogErrUrlExpired;
            }
        }
        publish_log_collect_error(cmd, reason);
        return;
    }

    std::filesystem::remove_all(work_dir, ec);
    publish_log_collect_accepted(cmd, static_cast<int64_t>(sealed.size()), reports);
    UPC_LOG_MQTT_SERVICE(Info,
        "log_collect done collectId=%s fileSize=%zu collected=%zu",
        cmd.collect_id.c_str(), sealed.size(), selected.size());
}

} // namespace upc::mqtt
