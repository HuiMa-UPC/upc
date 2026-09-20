#include "upload_manager.hpp"

#include "rs_log.hpp"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

namespace upc::xfr {

namespace {

/* 传输 3MF 会在板端展开；先限制压缩包本身，避免上传阶段耗尽 transport 分区。 */
constexpr uint64_t kMaxTransfer3mfBytes = 512ULL * 1024ULL * 1024ULL;

} // namespace

UploadManager::UploadManager(std::string storage_dir, uint32_t chunk_max_bytes)
    : storage_dir_(std::move(storage_dir)),
      chunk_max_(chunk_max_bytes ? chunk_max_bytes : (4U * 1024U * 1024U))
{
}

bool UploadManager::ensure_dirs()
{
    std::error_code ec;
    std::filesystem::create_directories(storage_dir_, ec);
    return !ec;
}

UploadInitResult UploadManager::init(const std::string& filename,
                                     uint64_t size,
                                     const std::string& sha256)
{
    UploadInitResult out;
    const std::string base = basename_only(filename);
    if (base.empty() || !is_supported_filename(base)) {
        out.err_code = "UNSUPPORTED_FILE_TYPE";
        out.err_msg = "only .ovf and .3mf files are allowed";
        out.http_status = 422;
        return out;
    }
    if (size == 0) {
        out.err_code = "BAD_REQUEST";
        out.err_msg = "size must be > 0";
        out.http_status = 400;
        return out;
    }
    const std::string lower_base = to_lower(base);
    const bool is_3mf =
        lower_base.size() > 4 &&
        lower_base.rfind(".3mf") == lower_base.size() - 4;
    if (is_3mf && size > kMaxTransfer3mfBytes) {
        out.err_code = "FILE_TOO_LARGE";
        out.err_msg = "3mf exceeds 512 MiB upload limit";
        out.http_status = 413;
        return out;
    }

    UploadSession s{};
    s.id = random_id(20);
    s.filename = base;
    s.expected_sha256 = to_lower(sha256);
    s.size = size;
    s.offset = 0;
    s.final_path = storage_dir_ + "/" + base;
    s.part_path = storage_dir_ + "/." + base + ".part." + s.id;

    {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_[s.id] = s;
    }

    std::ofstream touch(s.part_path, std::ios::binary | std::ios::trunc);
    if (!touch.good()) {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_.erase(s.id);
        out.err_code = "IO_ERROR";
        out.err_msg = "cannot create part file";
        out.http_status = 500;
        return out;
    }

    out.ok = true;
    out.upload_id = s.id;
    out.offset = 0;
    out.chunk_max = chunk_max_;
    return out;
}

UploadStatusResult UploadManager::status(const std::string& upload_id) const
{
    UploadStatusResult out;
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = sessions_.find(upload_id);
    if (it == sessions_.end()) {
        out.err_code = "UPLOAD_NOT_FOUND";
        out.err_msg = "upload not found";
        out.http_status = 404;
        return out;
    }
    out.ok = true;
    out.offset = it->second.offset;
    out.size = it->second.size;
    out.state = (it->second.offset >= it->second.size) ? "ready_complete" : "receiving";
    return out;
}

UploadStatusResult UploadManager::append_chunk(const std::string& upload_id,
                                               uint64_t start,
                                               uint64_t end,
                                               uint64_t total,
                                               const uint8_t* data,
                                               size_t len)
{
    UploadStatusResult out;
    if (!data || len == 0) {
        out.err_code = "BAD_REQUEST";
        out.err_msg = "empty body";
        out.http_status = 400;
        return out;
    }
    if (len > chunk_max_) {
        out.err_code = "CHUNK_TOO_LARGE";
        out.err_msg = "chunk exceeds chunk_max";
        out.http_status = 413;
        return out;
    }

    std::lock_guard<std::mutex> lk(mu_);
    const auto it = sessions_.find(upload_id);
    if (it == sessions_.end()) {
        out.err_code = "UPLOAD_NOT_FOUND";
        out.err_msg = "upload not found";
        out.http_status = 404;
        return out;
    }
    UploadSession& s = it->second;
    if (total != s.size) {
        out.err_code = "OFFSET_MISMATCH";
        out.err_msg = "total size mismatch";
        out.http_status = 422;
        return out;
    }
    if (start != s.offset) {
        out.err_code = "OFFSET_MISMATCH";
        out.err_msg = "unexpected chunk start";
        out.http_status = 422;
        return out;
    }
    if (end < start || (end - start + 1) != len || end >= s.size) {
        out.err_code = "OFFSET_MISMATCH";
        out.err_msg = "invalid content-range/body length";
        out.http_status = 422;
        return out;
    }

    std::ofstream ofs(s.part_path, std::ios::binary | std::ios::app);
    if (!ofs.good()) {
        out.err_code = "IO_ERROR";
        out.err_msg = "cannot append to part file";
        out.http_status = 500;
        return out;
    }
    ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    if (!ofs.good()) {
        out.err_code = "IO_ERROR";
        out.err_msg = "write part file failed";
        out.http_status = 500;
        return out;
    }
    s.offset += len;

    out.ok = true;
    out.offset = s.offset;
    out.size = s.size;
    out.state = (s.offset >= s.size) ? "ready_complete" : "receiving";
    return out;
}

UploadCompleteResult UploadManager::complete(const std::string& upload_id,
                                             const std::string& sha256)
{
    UploadCompleteResult out;
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = sessions_.find(upload_id);
    if (it == sessions_.end()) {
        out.err_code = "UPLOAD_NOT_FOUND";
        out.err_msg = "upload not found";
        out.http_status = 404;
        return out;
    }
    UploadSession s = it->second;
    if (s.offset != s.size) {
        out.err_code = "INCOMPLETE_UPLOAD";
        out.err_msg = "upload not finished";
        out.http_status = 409;
        return out;
    }
    std::string actual;
    if (!compute_file_sha256(s.part_path, &actual)) {
        out.err_code = "IO_ERROR";
        out.err_msg = "sha256 compute failed";
        out.http_status = 500;
        return out;
    }
    const std::string req_sha = to_lower(sha256);
    if (!req_sha.empty() && req_sha != actual) {
        std::filesystem::remove(s.part_path);
        sessions_.erase(it);
        out.err_code = "CHECKSUM_MISMATCH";
        out.err_msg = "request sha256 mismatch";
        out.http_status = 422;
        return out;
    }
    if (!s.expected_sha256.empty() && s.expected_sha256 != actual) {
        std::filesystem::remove(s.part_path);
        sessions_.erase(it);
        out.err_code = "CHECKSUM_MISMATCH";
        out.err_msg = "init sha256 mismatch";
        out.http_status = 422;
        return out;
    }

    std::error_code rm_ec;
    std::filesystem::remove(s.final_path, rm_ec);
    std::error_code mv_ec;
    std::filesystem::rename(s.part_path, s.final_path, mv_ec);
    if (mv_ec) {
        out.err_code = "IO_ERROR";
        out.err_msg = "rename part file failed";
        out.http_status = 500;
        return out;
    }
    sessions_.erase(it);

    out.ok = true;
    out.file_id = "f_" + s.id;
    out.file_name = s.filename;
    out.size = file_size_of(s.final_path);
    out.verified = true;
    UPC_LOG_XFR_SERVICE(Info, "received file ok: %s (%llu bytes)",
                        s.final_path.c_str(),
                        static_cast<unsigned long long>(out.size));
    return out;
}

bool UploadManager::is_supported_filename(const std::string& filename)
{
    const std::string lower = to_lower(filename);
    const bool is_ovf = lower.size() > 4 && lower.rfind(".ovf") == lower.size() - 4;
    const bool is_3mf = lower.size() > 4 && lower.rfind(".3mf") == lower.size() - 4;
    return is_ovf || is_3mf;
}

std::string UploadManager::basename_only(const std::string& path)
{
    std::string base = path;
    const std::size_t p = base.find_last_of("/\\");
    if (p != std::string::npos) {
        base = base.substr(p + 1);
    }
    if (base == "." || base == "..") {
        return {};
    }
    return base;
}

std::string UploadManager::to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string UploadManager::random_id(std::size_t n)
{
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(kHex[rng() & 0x0F]);
    }
    return out;
}

bool UploadManager::compute_file_sha256(const std::string& path, std::string* out_hex)
{
    if (!out_hex) {
        return false;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.good()) {
        return false;
    }
#if defined(OPENSSL_VERSION_MAJOR) && OPENSSL_VERSION_MAJOR >= 3
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        return false;
    }
    bool ok = EVP_DigestInit_ex(mdctx, EVP_sha256(), nullptr) == 1;
    char buf[8192];
    while (ok && ifs.good()) {
        ifs.read(buf, sizeof(buf));
        const std::streamsize n = ifs.gcount();
        if (n > 0) {
            ok = EVP_DigestUpdate(mdctx, buf, static_cast<std::size_t>(n)) == 1;
        }
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_len = 0;
    ok = ok && EVP_DigestFinal_ex(mdctx, digest, &digest_len) == 1 &&
         digest_len == SHA256_DIGEST_LENGTH;
    EVP_MD_CTX_free(mdctx);
    if (!ok) {
        return false;
    }
#else
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    char buf[8192];
    while (ifs.good()) {
        ifs.read(buf, sizeof(buf));
        const std::streamsize n = ifs.gcount();
        if (n > 0) {
            SHA256_Update(&ctx, buf, static_cast<std::size_t>(n));
        }
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);
#endif
    char hex[SHA256_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    hex[sizeof(hex) - 1] = '\0';
    *out_hex = hex;
    return true;
}

uint64_t UploadManager::file_size_of(const std::string& path)
{
    std::error_code ec;
    const auto s = std::filesystem::file_size(path, ec);
    if (ec) {
        return 0;
    }
    return static_cast<uint64_t>(s);
}

} // namespace upc::xfr
