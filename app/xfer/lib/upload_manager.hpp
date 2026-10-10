#ifndef UPC_XFR_UPLOAD_MANAGER_HPP
#define UPC_XFR_UPLOAD_MANAGER_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace upc::xfr {

struct UploadInitResult {
    bool ok = false;
    std::string upload_id;
    uint64_t offset = 0;
    uint32_t chunk_max = 0;
    std::string err_code;
    std::string err_msg;
    int http_status = 400;
};

struct UploadStatusResult {
    bool ok = false;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string state;
    std::string err_code;
    std::string err_msg;
    int http_status = 400;
};

struct UploadCompleteResult {
    bool ok = false;
    std::string file_id;
    uint64_t size = 0;
    bool verified = false;
    std::string file_name;
    std::string err_code;
    std::string err_msg;
    int http_status = 400;
};

class UploadManager {
public:
    UploadManager(std::string storage_dir, uint32_t chunk_max_bytes);

    bool ensure_dirs();
    UploadInitResult init(const std::string& filename, uint64_t size, const std::string& sha256);
    UploadStatusResult status(const std::string& upload_id) const;
    UploadStatusResult append_chunk(const std::string& upload_id,
                                    uint64_t start,
                                    uint64_t end,
                                    uint64_t total,
                                    const uint8_t* data,
                                    size_t len);
    UploadCompleteResult complete(const std::string& upload_id, const std::string& sha256);
    /** 丢掉所有未完成会话和 .part 文件。已 rename 落盘的完整包不动。 */
    void abort_all();

private:
    struct UploadSession {
        std::string id;
        std::string filename;
        std::string expected_sha256;
        std::string part_path;
        std::string final_path;
        uint64_t size = 0;
        uint64_t offset = 0;
    };

    static bool is_supported_filename(const std::string& filename);
    static std::string basename_only(const std::string& path);
    static std::string to_lower(std::string s);
    static std::string random_id(std::size_t n);
    static bool compute_file_sha256(const std::string& path, std::string* out_hex);
    static uint64_t file_size_of(const std::string& path);

    std::string storage_dir_;
    uint32_t chunk_max_ = 0;
    mutable std::mutex mu_;
    std::unordered_map<std::string, UploadSession> sessions_;
};

} // namespace upc::xfr

#endif
