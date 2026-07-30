#include "print_history_manifest.hpp"

#include "gcode_manifest.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace upc {

namespace {

constexpr const char* kEntriesKey = "\"entries\"";

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
    const std::string needle = std::string("\"") + key + "\":\"";
    const size_t pos = obj.find(needle);
    if(pos == std::string::npos) {
        return false;
    }
    size_t i = pos + needle.size();
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
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = obj.find(needle);
    if(pos == std::string::npos) {
        return false;
    }
    const char* start = obj.c_str() + pos + needle.size();
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
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = obj.find(needle);
    if(pos == std::string::npos) {
        return false;
    }
    const char* start = obj.c_str() + pos + needle.size();
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
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = obj.find(needle);
    if(pos == std::string::npos) {
        return false;
    }
    const char* start = obj.c_str() + pos + needle.size();
    char* end = nullptr;
    const double v = std::strtod(start, &end);
    if(end == start) {
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

bool parse_bool_field(const std::string& obj, const char* key, bool& out)
{
    const std::string needle = std::string("\"") + key + "\":";
    const size_t pos = obj.find(needle);
    if(pos == std::string::npos) {
        return false;
    }
    const char* start = obj.c_str() + pos + needle.size();
    if(std::strncmp(start, "true", 4) == 0) {
        out = true;
        return true;
    }
    if(std::strncmp(start, "false", 5) == 0) {
        out = false;
        return true;
    }
    char* end = nullptr;
    const unsigned long v = std::strtoul(start, &end, 10);
    if(end == start) {
        return false;
    }
    out = (v != 0);
    return true;
}

/** 去掉路径与扩展名，得到可与 job_name 比对的基名 */
std::string basename_no_ext(const std::string& name)
{
    size_t start = name.find_last_of('/');
    start = (start == std::string::npos) ? 0 : start + 1;
    size_t end = name.find_last_of('.');
    if(end == std::string::npos || end < start) {
        end = name.size();
    }
    return name.substr(start, end - start);
}

PrintHistoryEntry parse_object(const std::string& obj)
{
    PrintHistoryEntry e;
    parse_string_field(obj, "job_name", e.job_name);
    parse_int64_field(obj, "start_unix", e.start_unix);
    parse_uint_field(obj, "duration_sec", e.duration_sec);
    parse_float_field(obj, "material_g", e.material_g);
    std::string status_s;
    if(parse_string_field(obj, "status", status_s)) {
        (void)print_history_status_parse(status_s, e.status);
    }
    parse_string_field(obj, "frames_dir", e.frames_dir);
    parse_string_field(obj, "thumb", e.thumb);
    parse_string_field(obj, "video", e.video);
    (void)parse_bool_field(obj, "video_pending", e.video_pending);
    (void)parse_bool_field(obj, "committed", e.committed);
    (void)parse_uint_field(obj, "clean_first_png", e.clean_first_png);
    (void)parse_uint_field(obj, "clean_last_png", e.clean_last_png);
    return e;
}

} // namespace

const char* print_history_status_str(PrintHistoryStatus s)
{
    return s == PrintHistoryStatus::Terminated ? "terminated" : "complete";
}

bool print_history_status_parse(const std::string& s, PrintHistoryStatus& out)
{
    if(s == "terminated") {
        out = PrintHistoryStatus::Terminated;
        return true;
    }
    if(s == "complete") {
        out = PrintHistoryStatus::Complete;
        return true;
    }
    return false;
}

bool print_history_manifest_load(const std::string& path,
                                 std::vector<PrintHistoryEntry>& out)
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

    const size_t entries_pos = text.find(kEntriesKey);
    if(entries_pos == std::string::npos) {
        return false;
    }
    const size_t arr_start = text.find('[', entries_pos);
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
        PrintHistoryEntry e = parse_object(obj);
        if(!e.frames_dir.empty() || !e.job_name.empty()) {
            out.push_back(std::move(e));
        }
        i = obj_end + 1;
    }
    return true;
}

bool print_history_manifest_save(const std::string& path,
                                 const std::vector<PrintHistoryEntry>& entries)
{
    std::ofstream out(path, std::ios::trunc);
    if(!out) {
        return false;
    }
    out << "{\"entries\":[";
    for(size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if(i > 0) {
            out << ',';
        }
        out << '{'
            << "\"job_name\":\"" << json_escape(e.job_name) << "\","
            << "\"start_unix\":" << e.start_unix << ','
            << "\"duration_sec\":" << e.duration_sec << ','
            << "\"material_g\":" << e.material_g << ','
            << "\"status\":\"" << print_history_status_str(e.status) << "\","
            << "\"frames_dir\":\"" << json_escape(e.frames_dir) << "\","
            << "\"thumb\":\"" << json_escape(e.thumb) << "\","
            << "\"video\":\"" << json_escape(e.video) << "\","
            << "\"video_pending\":" << (e.video_pending ? "true" : "false") << ","
            << "\"committed\":" << (e.committed ? "true" : "false") << ","
            << "\"clean_first_png\":" << e.clean_first_png << ','
            << "\"clean_last_png\":" << e.clean_last_png
            << '}';
    }
    out << "]}";
    return static_cast<bool>(out);
}

void print_history_manifest_upsert(std::vector<PrintHistoryEntry>& entries,
                                   const PrintHistoryEntry& item)
{
    for(auto& e : entries) {
        if(e.frames_dir == item.frames_dir && !item.frames_dir.empty()) {
            e = item;
            return;
        }
    }
    entries.push_back(item);
}

float print_history_lookup_material_g(const std::string& gcode_config_path,
                                      const std::string& job_basename)
{
    if(job_basename.empty()) {
        return 0.f;
    }
    std::vector<GcodeManifestEntry> files;
    if(!gcode_manifest_load(gcode_config_path, files)) {
        return 0.f;
    }
    for(const auto& e : files) {
        if(e.name == job_basename || basename_no_ext(e.name) == job_basename) {
            return e.material_g;
        }
    }
    return 0.f;
}

} // namespace upc
