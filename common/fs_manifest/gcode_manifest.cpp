#include "gcode_manifest.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace upc {

namespace {

constexpr const char* kFilesKey = "\"files\"";

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for(char c : s) {
        if(c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

bool parse_string_field(const std::string& obj, const char* key, std::string& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    const size_t key_pos = obj.find(needle);
    if(key_pos == std::string::npos) {
        return false;
    }

    size_t i = key_pos + needle.size();
    while(i < obj.size() && std::isspace(static_cast<unsigned char>(obj[i]))) {
        ++i;
    }
    if(i >= obj.size() || obj[i] != ':') {
        return false;
    }
    ++i;
    while(i < obj.size() && std::isspace(static_cast<unsigned char>(obj[i]))) {
        ++i;
    }
    if(i >= obj.size() || obj[i] != '"') {
        return false;
    }
    ++i;

    out.clear();
    while(i < obj.size()) {
        if(obj[i] == '\\' && i + 1 < obj.size()) {
            out.push_back(obj[i + 1]);
            i += 2;
            continue;
        }
        if(obj[i] == '"') {
            return true;
        }
        out.push_back(obj[i]);
        ++i;
    }
    return false;
}

bool parse_uint_field(const std::string& obj, const char* key, uint32_t& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    const size_t key_pos = obj.find(needle);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t i = key_pos + needle.size();
    while(i < obj.size() && std::isspace(static_cast<unsigned char>(obj[i]))) {
        ++i;
    }
    if(i >= obj.size() || obj[i] != ':') {
        return false;
    }
    ++i;
    while(i < obj.size() && std::isspace(static_cast<unsigned char>(obj[i]))) {
        ++i;
    }
    if(i >= obj.size()) {
        return false;
    }
    const char* start = obj.c_str() + i;
    char* end = nullptr;
    const unsigned long v = std::strtoul(start, &end, 10);
    if(end == start) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool parse_int64_field(const std::string& obj, const char* key, int64_t& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    const size_t key_pos = obj.find(needle);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t i = key_pos + needle.size();
    while(i < obj.size() && std::isspace(static_cast<unsigned char>(obj[i]))) {
        ++i;
    }
    if(i >= obj.size() || obj[i] != ':') {
        return false;
    }
    ++i;
    while(i < obj.size() && std::isspace(static_cast<unsigned char>(obj[i]))) {
        ++i;
    }
    if(i >= obj.size()) {
        return false;
    }
    const char* start = obj.c_str() + i;
    char* end = nullptr;
    const long long v = std::strtoll(start, &end, 10);
    if(end == start) {
        return false;
    }
    out = v;
    return true;
}

bool parse_float_field(const std::string& obj, const char* key, float& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    const size_t key_pos = obj.find(needle);
    if(key_pos == std::string::npos) {
        return false;
    }
    size_t i = key_pos + needle.size();
    while(i < obj.size() && std::isspace(static_cast<unsigned char>(obj[i]))) {
        ++i;
    }
    if(i >= obj.size() || obj[i] != ':') {
        return false;
    }
    ++i;
    while(i < obj.size() && std::isspace(static_cast<unsigned char>(obj[i]))) {
        ++i;
    }
    if(i >= obj.size()) {
        return false;
    }
    const char* start = obj.c_str() + i;
    char* end = nullptr;
    const double v = std::strtod(start, &end);
    if(end == start) {
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

GcodeManifestEntry parse_object(const std::string& obj)
{
    GcodeManifestEntry e;
    parse_string_field(obj, "name", e.name);
    parse_uint_field(obj, "file_size", e.file_size);
    parse_uint_field(obj, "line_count", e.line_count);
    parse_uint_field(obj, "layer_count", e.layer_count);
    parse_uint_field(obj, "duration_sec", e.duration_sec);
    parse_float_field(obj, "material_g", e.material_g);
    parse_int64_field(obj, "mtime", e.mtime);
    parse_string_field(obj, "thumb_small", e.thumb_small);
    parse_string_field(obj, "thumb_large", e.thumb_large);
    parse_string_field(obj, "thumb_print", e.thumb_print);
    return e;
}

} // namespace

bool gcode_manifest_load(const std::string& path, std::vector<GcodeManifestEntry>& out)
{
    out.clear();
    std::ifstream in(path);
    if(!in) {
        return true;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    if(text.empty()) {
        return true;
    }

    const size_t files_pos = text.find(kFilesKey);
    if(files_pos == std::string::npos) {
        return false;
    }
    const size_t arr_start = text.find('[', files_pos);
    if(arr_start == std::string::npos) {
        return false;
    }

    size_t i = arr_start + 1;
    while(i < text.size()) {
        const size_t obj_start = text.find('{', i);
        if(obj_start == std::string::npos) {
            break;
        }
        const size_t obj_end = text.find('}', obj_start);
        if(obj_end == std::string::npos) {
            break;
        }
        const std::string obj = text.substr(obj_start, obj_end - obj_start + 1);
        GcodeManifestEntry e = parse_object(obj);
        if(!e.name.empty()) {
            out.push_back(std::move(e));
        }
        i = obj_end + 1;
    }
    return true;
}

bool gcode_manifest_save(const std::string& path,
                         const std::vector<GcodeManifestEntry>& entries)
{
    const std::string tmp_path = path + ".tmp";
    std::ofstream out(tmp_path, std::ios::trunc);
    if(!out) {
        return false;
    }
    out << "{\n"
        << "  \"files\": [\n";
    for(size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        out << "    {\n"
            << "      \"name\": \"" << json_escape(e.name) << "\",\n"
            << "      \"file_size\": " << e.file_size << ",\n"
            << "      \"line_count\": " << e.line_count << ",\n"
            << "      \"layer_count\": " << e.layer_count << ",\n"
            << "      \"duration_sec\": " << e.duration_sec << ",\n"
            << "      \"material_g\": " << e.material_g << ",\n"
            << "      \"mtime\": " << e.mtime << ",\n"
            << "      \"thumb_small\": \"" << json_escape(e.thumb_small) << "\",\n"
            << "      \"thumb_large\": \"" << json_escape(e.thumb_large) << "\",\n"
            << "      \"thumb_print\": \"" << json_escape(e.thumb_print) << "\"\n"
            << "    }";
        if(i + 1 < entries.size()) {
            out << ",";
        }
        out << "\n";
    }
    out << "  ]\n"
        << "}\n";
    if(!out) {
        out.close();
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    out.close();
    if(!out) {
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    if(std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        (void)std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

void gcode_manifest_upsert(std::vector<GcodeManifestEntry>& entries,
                           const GcodeManifestEntry& item)
{
    for(auto& e : entries) {
        if(e.name == item.name) {
            const std::string old_small = e.thumb_small;
            const std::string old_large = e.thumb_large;
            const std::string old_print = e.thumb_print;
            const uint32_t old_layer_count = e.layer_count;
            e = item;
            /* prepare/xfer 目前不会产缩略图，保持已有路径不被覆盖丢失。 */
            if(e.thumb_small.empty()) e.thumb_small = old_small;
            if(e.thumb_large.empty()) e.thumb_large = old_large;
            if(e.thumb_print.empty()) e.thumb_print = old_print;
            /* 兼容旧写入方：若未提供 layer_count，保留已有缓存层数。 */
            if(e.layer_count == 0 && old_layer_count > 0) {
                e.layer_count = old_layer_count;
            }
            return;
        }
    }
    entries.push_back(item);
}

bool gcode_manifest_remove_at(std::vector<GcodeManifestEntry>& entries, size_t index)
{
    if(index >= entries.size()) {
        return false;
    }
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

} // namespace upc
