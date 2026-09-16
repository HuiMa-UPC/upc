#ifndef UPC_XFR_TRANSFER_THUMBNAIL_GENERATOR_HPP
#define UPC_XFR_TRANSFER_THUMBNAIL_GENERATOR_HPP

#include <array>
#include <cstdint>
#include <string>

namespace upc::xfr {

struct TransferThumbnailTarget {
    std::string path;
    uint32_t width = 0;
    uint32_t height = 0;
};

/**
 * 将传输 3MF 中提取的 PNG 转为 UI 可直接读取的三份 RGB565 RTHM。
 *
 * 几何规则与 tools/img2rthm_triplet.py 一致：按目标比例居中 COVER 裁剪。
 * 有 librga 时优先尝试 RGA；普通内存无法导入或硬件转换失败时自动走 CPU。
 */
bool generate_transfer_thumbnail_triplet(
    const std::string& png_path,
    const std::array<TransferThumbnailTarget, 3>& targets,
    std::string* error);

} // namespace upc::xfr

#endif /* UPC_XFR_TRANSFER_THUMBNAIL_GENERATOR_HPP */
