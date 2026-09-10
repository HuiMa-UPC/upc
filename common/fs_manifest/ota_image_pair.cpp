#include "ota_image_pair.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace upc {

namespace {

const char* prefix_for_kind(OtaImageKind kind)
{
    return (kind == OtaImageKind::Mcu) ? "MainBoard_Slot" : "Galvo_Slot";
}

std::string basename_only(const std::string& path)
{
    const size_t pos = path.find_last_of('/');
    if(pos == std::string::npos) {
        return path;
    }
    return path.substr(pos + 1);
}

std::string strip_leading_dot_slash(std::string name)
{
    while(name.size() >= 2 && name[0] == '.' && name[1] == '/') {
        name.erase(0, 2);
    }
    return name;
}

struct SlotPaths {
    std::string a;
    std::string b;
};

} // namespace

bool ota_image_kind_from_component(const std::string& component, OtaImageKind& out)
{
    if(component == "mcu") {
        out = OtaImageKind::Mcu;
        return true;
    }
    if(component == "galvo") {
        out = OtaImageKind::Galvo;
        return true;
    }
    return false;
}

bool parse_ota_image_basename(const std::string& basename,
                              OtaImageKind kind,
                              OtaImageSlot& slot,
                              OtaImageVersion& version)
{
    const std::string name = strip_leading_dot_slash(basename);
    if(name.empty() || name.find('/') != std::string::npos ||
       name.find('\\') != std::string::npos) {
        return false;
    }

    const char* prefix = prefix_for_kind(kind);
    const size_t plen = std::strlen(prefix);
    if(name.size() <= plen || name.compare(0, plen, prefix) != 0) {
        return false;
    }

    const char* p = name.c_str() + plen;
    if(*p == 'A') {
        slot = OtaImageSlot::A;
    } else if(*p == 'B') {
        slot = OtaImageSlot::B;
    } else {
        return false;
    }
    ++p;
    if(p[0] != '_' || p[1] != 'V') {
        return false;
    }
    p += 2;

    unsigned major = 0;
    unsigned minor = 0;
    unsigned patch = 0;
    int consumed = 0;
    if(std::sscanf(p, "%u.%u.%u.bin%n", &major, &minor, &patch, &consumed) != 3) {
        return false;
    }
    if(consumed <= 0 || p[consumed] != '\0') {
        return false;
    }

    version.major = major;
    version.minor = minor;
    version.patch = patch;
    return true;
}

OtaImagePairError select_highest_ota_image_pair(
    const std::vector<std::string>& paths,
    OtaImageKind kind,
    OtaImagePair& out)
{
    out = OtaImagePair{};
    if(paths.empty()) {
        return OtaImagePairError::Empty;
    }

    std::map<OtaImageVersion, SlotPaths> groups;
    for(const std::string& path : paths) {
        OtaImageSlot slot = OtaImageSlot::A;
        OtaImageVersion ver{};
        if(!parse_ota_image_basename(basename_only(path), kind, slot, ver)) {
            return OtaImagePairError::InvalidName;
        }
        SlotPaths& g = groups[ver];
        std::string& dst = (slot == OtaImageSlot::A) ? g.a : g.b;
        if(!dst.empty()) {
            return OtaImagePairError::DuplicateSlot;
        }
        dst = path;
    }

    for(const auto& entry : groups) {
        if(entry.second.a.empty() || entry.second.b.empty()) {
            return OtaImagePairError::IncompletePair;
        }
    }

    const auto highest = groups.rbegin();
    out.version = highest->first;
    out.slot_a_path = highest->second.a;
    out.slot_b_path = highest->second.b;
    return OtaImagePairError::Ok;
}

OtaImagePairError scan_highest_ota_image_pair(const char* dir,
                                              OtaImageKind kind,
                                              OtaImagePair& out)
{
    out = OtaImagePair{};
    if(!dir || dir[0] == '\0') {
        return OtaImagePairError::ScanFailed;
    }

    DIR* dp = ::opendir(dir);
    if(!dp) {
        return OtaImagePairError::ScanFailed;
    }

    std::string prefix = dir;
    if(!prefix.empty() && prefix.back() != '/') {
        prefix += '/';
    }

    std::vector<std::string> paths;
    for(dirent* ent = ::readdir(dp); ent; ent = ::readdir(dp)) {
        if(ent->d_name[0] == '.' &&
           (ent->d_name[1] == '\0' ||
            (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) {
            continue;
        }
        const std::string path = prefix + ent->d_name;
        struct stat st{};
        if(::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        paths.push_back(path);
    }
    ::closedir(dp);

    return select_highest_ota_image_pair(paths, kind, out);
}

const char* ota_image_pair_error_cstr(OtaImagePairError err)
{
    switch(err) {
    case OtaImagePairError::Ok:             return "ok";
    case OtaImagePairError::Empty:          return "empty";
    case OtaImagePairError::InvalidName:    return "invalid_name";
    case OtaImagePairError::IncompletePair: return "incomplete_pair";
    case OtaImagePairError::DuplicateSlot:  return "duplicate_slot";
    case OtaImagePairError::ScanFailed:     return "scan_failed";
    default:                                return "unknown";
    }
}

} // namespace upc
