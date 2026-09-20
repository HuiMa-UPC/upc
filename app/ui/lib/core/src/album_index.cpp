#include "album_index.hpp"

#include "media_paths.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <filesystem>

namespace ui::album {

bool is_photo_file(const std::string& path)
{
    const std::string ext = std::filesystem::path(path).extension().string();
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".webp" || ext == ".bmp";
}

std::string companion_rthm(const std::string& photo_abs_path)
{
    if(photo_abs_path.empty()) {
        return {};
    }
    const std::filesystem::path p(photo_abs_path);
    const std::string ext = p.extension().string();
    if(ext != ".png" && ext != ".PNG") {
        return {};
    }
    return p.parent_path().string() + "/" + p.stem().string() + ".rthm";
}

namespace {

/** 文件 mtime（Unix 秒）；取不到返回 0。
 *
 *  用 ::stat 而不是 std::filesystem::last_write_time：后者返回的 file_time_type
 *  在 C++17 里没有到 time_t 的可移植转换（libstdc++ 的时钟基准还随版本变过），
 *  而历史页要拿它 strftime 出「May 2026」和「22:03:59」。同一处理见 glavo_service。 */
int64_t file_mtime_unix(const std::string& path)
{
    struct stat st {};
    if(::stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<int64_t>(st.st_mtime);
}

} /* namespace */

std::vector<PhotoEntry> scan()
{
    std::vector<PhotoEntry> out;
    std::error_code ec;
    if(std::filesystem::is_directory(UPC_PHOTO_DIR, ec)) {
        for(const auto& de : std::filesystem::directory_iterator(UPC_PHOTO_DIR, ec)) {
            if(ec) {
                break;
            }
            if(!de.is_regular_file(ec) || ec) {
                continue;
            }
            const std::string path = de.path().string();
            if(!is_photo_file(path)) {
                continue;
            }
            PhotoEntry e;
            e.path = path;
            /* mtime 在这里读一次就存下：早先是在排序的比较器里现读，同一个文件会被
             * stat 上 O(log n) 次，而且排完之后那个值就丢了。 */
            e.mtime_unix = file_mtime_unix(path);
            /* 缩略图优先同名 .rthm（Media 拍照时从 NV12 生成），无则留空由调用方回退原图 */
            const std::string rthm = companion_rthm(path);
            std::error_code te;
            if(!rthm.empty() && std::filesystem::exists(rthm, te) && !te) {
                e.thumb = rthm;
            }
            out.push_back(std::move(e));
        }
    }

    /* 新的在前。mtime 取不到（0）的排到最后，再按文件名倒序兜底，保证顺序稳定——
     * 全屏浏览器的「下一张」用的是同一份顺序，两处错位就会跳图。 */
    std::sort(out.begin(), out.end(),
        [](const PhotoEntry& a, const PhotoEntry& b) {
            if(a.mtime_unix != b.mtime_unix) {
                return a.mtime_unix > b.mtime_unix;
            }
            return a.path > b.path;
        });
    return out;
}

std::size_t index_of(const std::vector<PhotoEntry>& list, const std::string& path)
{
    for(std::size_t i = 0; i < list.size(); ++i) {
        if(list[i].path == path) {
            return i;
        }
    }
    return kInvalidIndex;
}

} // namespace ui::album
