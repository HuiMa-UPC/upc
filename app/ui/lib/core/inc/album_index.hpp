#ifndef UI_ALBUM_INDEX_HPP
#define UI_ALBUM_INDEX_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace ui::album {

/** 找不到时的下标返回值 */
constexpr std::size_t kInvalidIndex = static_cast<std::size_t>(-1);

/** 相册一条：图片本体 + 配对缩略图 + 拍摄时间 */
struct PhotoEntry {
    std::string path;   /**< 图片绝对路径 */
    std::string thumb;  /**< 配对 .rthm 绝对路径；空 = 无，需回退解原图 */
    /** 文件 mtime（Unix 秒）。照片没有清单、文件名里也不带时间，这是唯一的时间来源；
     *  历史页拿它显示卡片角上的时间戳、并按月分组。取不到时为 0，调用方按「未知」处理。 */
    int64_t     mtime_unix = 0;
};

/**
 * 相册收录的扩展名。
 *
 * 注意 `.webp` 在当前 lv_conf（只开了 LODEPNG / TJPGD / BMP）下**没有解码器**，
 * 收录它是为了保持历史页原有行为不变；真出现 webp 时缩略图与大图都会解失败，
 * 由调用方按「解不出来」的分支兜住。
 */
bool is_photo_file(const std::string& path);

/** PNG 旁路 RTHM：/userdata/media/photo/foo.png → …/foo.rthm（非 PNG 返回空串） */
std::string companion_rthm(const std::string& photo_abs_path);

/**
 * 扫 UPC_PHOTO_DIR，按 mtime 倒序（取不到 mtime 时回退按文件名倒序）。
 *
 * 历史页相册列表与全屏浏览器**必须共用此函数**：两处若各自排序，一旦口径飘掉
 * 浏览器里的「上一张 / 下一张」就会和列表看到的顺序错位。
 */
std::vector<PhotoEntry> scan();

/** 在 list 里找 path 的下标；找不到返回 kInvalidIndex */
std::size_t index_of(const std::vector<PhotoEntry>& list, const std::string& path);

} // namespace ui::album

#endif /* UI_ALBUM_INDEX_HPP */
