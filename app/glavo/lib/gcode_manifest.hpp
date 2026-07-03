#ifndef GCODE_MANIFEST_HPP
#define GCODE_MANIFEST_HPP

/** Local 目录 manifest：/home/Gcode/config.json（UI Local 列表唯一数据源） */

#include <cstdint>
#include <string>
#include <vector>

namespace upc::glavo {

struct GcodeManifestEntry {
    std::string name;
    uint32_t    file_size    = 0;
    uint32_t    line_count   = 0;
    uint32_t    duration_sec = 0;
    float       material_g   = 0.f;
    int64_t     mtime        = 0;
};

/** 从 path 读取 config.json；文件不存在返回空列表 */
bool gcode_manifest_load(const std::string& path, std::vector<GcodeManifestEntry>& out);

/** 写入 config.json（覆盖） */
bool gcode_manifest_save(const std::string& path,
                         const std::vector<GcodeManifestEntry>& entries);

/** 按文件名 upsert；不存在则追加 */
void gcode_manifest_upsert(std::vector<GcodeManifestEntry>& entries,
                           const GcodeManifestEntry& item);

/** 按 index 删除 manifest 项；越界返回 false */
bool gcode_manifest_remove_at(std::vector<GcodeManifestEntry>& entries, size_t index);

} // namespace upc::glavo

#endif
