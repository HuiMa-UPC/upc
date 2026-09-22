#include "album_index.hpp"

#include "media_paths.hpp"

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

std::vector<PhotoEntry> scan()
{
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    if(std::filesystem::is_directory(UPC_PHOTO_DIR, ec)) {
        for(const auto& de : std::filesystem::directory_iterator(UPC_PHOTO_DIR, ec)) {
            if(ec) {
                break;
            }
            if(!de.is_regular_file(ec) || ec) {
                continue;
            }
            if(is_photo_file(de.path().string())) {
                paths.push_back(de.path());
            }
        }
    }

    std::sort(paths.begin(), paths.end(),
        [](const std::filesystem::path& a, const std::filesystem::path& b) {
            std::error_code ea;
            std::error_code eb;
            const auto ta = std::filesystem::last_write_time(a, ea);
            const auto tb = std::filesystem::last_write_time(b, eb);
            if(ea || eb) {
                return a.filename().string() > b.filename().string();
            }
            return ta > tb;
        });

    std::vector<PhotoEntry> out;
    out.reserve(paths.size());
    for(const auto& p : paths) {
        PhotoEntry e;
        e.path = p.string();
        /* 缩略图优先同名 .rthm（Media 拍照时从 NV12 生成），无则留空由调用方回退原图 */
        const std::string rthm = companion_rthm(e.path);
        std::error_code te;
        if(!rthm.empty() && std::filesystem::exists(rthm, te) && !te) {
            e.thumb = rthm;
        }
        out.push_back(std::move(e));
    }
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
