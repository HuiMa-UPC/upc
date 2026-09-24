#include "cabin_profile.hpp"
#include "cabin_detector.hpp"

#include <filesystem>
#include <system_error>

namespace upc::visual {
bool load_cabin_profile(CabinDetector& detector, bool door,
                        const char* override_path, const std::string& station_path,
                        std::string& source, std::string& error) {
    const auto load_file = [&](const std::string& path, const char* kind) {
        source = std::string(kind) + path;
        return door ? detector.load_door_config(path,error)
                    : detector.load_plate_config(path,error);
    };
    if(override_path && *override_path) return load_file(override_path,"override:");

    // symlink_status intentionally recognizes a dangling link as an existing
    // override. Permission errors and non-file paths likewise must not enable
    // the default profile behind the engineer's back.
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(station_path,ec);
    const bool absent = status.type() == std::filesystem::file_type::not_found &&
        (!ec || ec == std::errc::no_such_file_or_directory);
    if(!absent) return load_file(station_path,"station:");

    source = std::string("bundled:") + cabin_bundled_profile_id();
    return door ? detector.load_bundled_door_config(error)
                : detector.load_bundled_plate_config(error);
}
} // namespace upc::visual
