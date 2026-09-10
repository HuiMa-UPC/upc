#ifndef UPC_OTA_IMAGE_PAIR_HPP
#define UPC_OTA_IMAGE_PAIR_HPP

/**
 * MCU / Galvo OTA 镜像 AB 对：命名解析与最高版本选取。
 * 规则正文见 common/fs_manifest/OTA_IMAGE.md。
 */

#include <cstdint>
#include <string>
#include <vector>

namespace upc {

enum class OtaImageKind : uint8_t {
    Mcu   = 0, /**< /userdata/image/mcu ：MainBoard_Slot{A|B}_V*.bin */
    Galvo = 1, /**< /userdata/image/galvo：Galvo_Slot{A|B}_V*.bin */
};

enum class OtaImageSlot : uint8_t {
    A = 0,
    B = 1,
};

enum class OtaImagePairError : uint8_t {
    Ok             = 0,
    Empty          = 1, /**< 目录无常规文件 */
    InvalidName    = 2, /**< 文件名不符合对应前缀/版本格式 */
    IncompletePair = 3, /**< 某版本缺 A 或缺 B */
    DuplicateSlot  = 4, /**< 同版本同槽位多于一个文件 */
    ScanFailed     = 5, /**< opendir 失败 */
};

struct OtaImageVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
};

inline bool operator<(const OtaImageVersion& a, const OtaImageVersion& b)
{
    if(a.major != b.major) {
        return a.major < b.major;
    }
    if(a.minor != b.minor) {
        return a.minor < b.minor;
    }
    return a.patch < b.patch;
}

struct OtaImagePair {
    std::string      slot_a_path;
    std::string      slot_b_path;
    OtaImageVersion  version;
};

/** component 字符串 mcu/galvo → 镜像种类。 */
bool ota_image_kind_from_component(const std::string& component, OtaImageKind& out);

/** 只解析 basename（可含 ./ 前缀，不含目录分隔）。 */
bool parse_ota_image_basename(const std::string& basename,
                              OtaImageKind kind,
                              OtaImageSlot& slot,
                              OtaImageVersion& version);

/** 从路径列表选取：所有组必须成对，只返回版本最高的 SlotA、SlotB。 */
OtaImagePairError select_highest_ota_image_pair(
    const std::vector<std::string>& paths,
    OtaImageKind kind,
    OtaImagePair& out);

/** 扫描目录下全部常规文件后调用 select_highest_ota_image_pair。 */
OtaImagePairError scan_highest_ota_image_pair(const char* dir,
                                              OtaImageKind kind,
                                              OtaImagePair& out);

const char* ota_image_pair_error_cstr(OtaImagePairError err);

} // namespace upc

#endif
