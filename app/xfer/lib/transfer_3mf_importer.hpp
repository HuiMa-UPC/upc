#ifndef UPC_XFR_TRANSFER_3MF_IMPORTER_HPP
#define UPC_XFR_TRANSFER_3MF_IMPORTER_HPP

#include <cstdint>
#include <mutex>
#include <string>

namespace upc::xfr {

/**
 * 传输 3MF 导入结果。
 *
 * 3MF 只作为 ZIP 容器；成功后 printable_name 指向已经落到
 * UPC_LOCAL_GCODE_DIR、可直接交给 GcodeWorker 的平铺 .gcode 文件。
 */
struct Transfer3mfImportResult {
    bool ok = false;
    int http_status = 422;
    std::string err_code;
    std::string err_msg;
    std::string printable_name;
    uint64_t printable_size = 0;
    uint32_t line_count = 0;
    uint32_t layer_count = 0;
};

/**
 * 将一个只含单份已切片 G-code 的传输 3MF 导入 Local 打印目录。
 *
 * 支持客户端现行的 3D 目录 G-code，也兼容早期协议草案的
 * Metadata 目录 G-code。实现会校验 ZIP 路径、条目数量、压缩方法、CRC、
 * 解压大小与压缩比，并通过临时文件 + rename 原子替换最终 G-code。
 * 必须读取 Metadata/*.json（snowpath_plate_meta_v1）的 layers /
 * print_time_s，写入 manifest 的 layer_count / duration_sec；line_count
 * 固定为 0（打印走纯层进度）。可选的 Thumbnails PNG 仅作解码缓存，
 * 转成三份 RTHM 后写入 UPC_LOCAL_GCODE_THUMB_DIR（/userdata/Gcode/thumb），
 * 不保留 PNG；图片失败只降级为空缩略图，不影响有效 G-code 导入。
 */
class Transfer3mfImporter {
public:
    explicit Transfer3mfImporter(std::string gcode_dir,
                                 uint64_t max_gcode_bytes = 1024ULL * 1024ULL * 1024ULL);

    Transfer3mfImportResult import_file(const std::string& package_path,
                                        const std::string& package_name) const;

private:
    std::string gcode_dir_;
    uint64_t max_gcode_bytes_;
    mutable std::mutex import_mu_; /**< 串行提交 G-code 与共享 config.json。 */
};

} // namespace upc::xfr

#endif
