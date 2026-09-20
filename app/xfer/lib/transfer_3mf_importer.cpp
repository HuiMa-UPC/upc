#include "transfer_3mf_importer.hpp"
#include "transfer_thumbnail_generator.hpp"

#include "gcode_manifest.hpp"
#include "rs_log.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace upc::xfr {

namespace {

constexpr uint32_t kLocalHeaderSignature = 0x04034b50U;
constexpr uint32_t kCentralHeaderSignature = 0x02014b50U;
constexpr uint32_t kEocdSignature = 0x06054b50U;
constexpr uint32_t kZip64EocdSignature = 0x06064b50U;
constexpr uint32_t kZip64LocatorSignature = 0x07064b50U;
constexpr uint16_t kZip64ExtraId = 0x0001U;
constexpr uint64_t kMaxArchiveEntries = 256;
constexpr uint64_t kMaxCompressionRatio = 200;
constexpr uint64_t kMaxThumbnailPngBytes = 32ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxMetadataJsonBytes = 1ULL * 1024ULL * 1024ULL;
constexpr uint64_t kEocdSearchBytes = 65557; /* EOCD 22 字节 + 最长 65535 字节注释 */

struct ZipEntry {
    std::string name;
    uint16_t flags = 0;
    uint16_t method = 0;
    uint32_t crc32 = 0;
    uint64_t compressed_size = 0;
    uint64_t uncompressed_size = 0;
    uint64_t local_header_offset = 0;
    uint32_t external_attributes = 0;
};

uint16_t read_u16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_u64(const uint8_t* p)
{
    return static_cast<uint64_t>(read_u32(p)) |
           (static_cast<uint64_t>(read_u32(p + 4)) << 32);
}

bool read_at(std::ifstream& in, uint64_t offset, void* dst, size_t size)
{
    if(offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if(!in) {
        return false;
    }
    in.read(static_cast<char*>(dst), static_cast<std::streamsize>(size));
    return in.gcount() == static_cast<std::streamsize>(size);
}

std::string lower_ascii(std::string value)
{
    for(char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool ends_with(const std::string& text, const std::string& suffix)
{
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool starts_with(const std::string& text, const std::string& prefix)
{
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

/**
 * ZIP 条目必须是相对路径，且不允许 "." / ".." 分量或反斜杠。
 * 当前虽然只提取 G-code，但先校验整个目录，避免后续扩展误用危险条目。
 */
bool archive_entry_path_safe(const std::string& name)
{
    if(name.empty() || name.front() == '/' || name.front() == '\\' ||
       name.find('\\') != std::string::npos ||
       (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) &&
        name[1] == ':')) {
        return false;
    }

    size_t begin = 0;
    while(begin <= name.size()) {
        const size_t slash = name.find('/', begin);
        const size_t end = (slash == std::string::npos) ? name.size() : slash;
        const std::string_view part(name.data() + begin, end - begin);
        if(part == "." || part == "..") {
            return false;
        }
        if(slash == std::string::npos) {
            break;
        }
        begin = slash + 1;
    }
    return true;
}

bool is_gcode_entry(const std::string& name)
{
    const std::string lower = lower_ascii(name);
    if(!ends_with(lower, ".gcode")) {
        return false;
    }
    return starts_with(lower, "3d/") || starts_with(lower, "metadata/");
}

bool is_thumbnail_png_entry(const std::string& name)
{
    const std::string lower = lower_ascii(name);
    return starts_with(lower, "thumbnails/") && ends_with(lower, ".png");
}

bool is_metadata_json_entry(const std::string& name)
{
    const std::string lower = lower_ascii(name);
    return starts_with(lower, "metadata/") && ends_with(lower, ".json");
}

bool parse_zip64_extra(const std::vector<uint8_t>& extra,
                       bool need_uncompressed,
                       bool need_compressed,
                       bool need_offset,
                       uint64_t* uncompressed,
                       uint64_t* compressed,
                       uint64_t* offset)
{
    size_t pos = 0;
    while(pos + 4 <= extra.size()) {
        const uint16_t id = read_u16(extra.data() + pos);
        const uint16_t len = read_u16(extra.data() + pos + 2);
        pos += 4;
        if(pos + len > extra.size()) {
            return false;
        }
        if(id != kZip64ExtraId) {
            pos += len;
            continue;
        }

        size_t field = pos;
        const size_t end = pos + len;
        const auto take_u64 = [&](uint64_t* out) -> bool {
            if(field + 8 > end) {
                return false;
            }
            *out = read_u64(extra.data() + field);
            field += 8;
            return true;
        };
        if(need_uncompressed && !take_u64(uncompressed)) {
            return false;
        }
        if(need_compressed && !take_u64(compressed)) {
            return false;
        }
        if(need_offset && !take_u64(offset)) {
            return false;
        }
        return true;
    }
    return !(need_uncompressed || need_compressed || need_offset);
}

bool read_zip_directory(std::ifstream& in,
                        uint64_t file_size,
                        std::vector<ZipEntry>* entries,
                        std::string* error)
{
    if(file_size < 22) {
        *error = "3mf is too small to be a ZIP archive";
        return false;
    }

    const uint64_t search_size = std::min(file_size, kEocdSearchBytes);
    std::vector<uint8_t> tail(static_cast<size_t>(search_size));
    if(!read_at(in, file_size - search_size, tail.data(), tail.size())) {
        *error = "cannot read ZIP end record";
        return false;
    }

    size_t eocd_in_tail = std::string::npos;
    for(size_t i = tail.size() - 22;; --i) {
        if(read_u32(tail.data() + i) == kEocdSignature) {
            const uint16_t comment_len = read_u16(tail.data() + i + 20);
            if(i + 22 + comment_len == tail.size()) {
                eocd_in_tail = i;
                break;
            }
        }
        if(i == 0) {
            break;
        }
    }
    if(eocd_in_tail == std::string::npos) {
        *error = "ZIP end record not found";
        return false;
    }

    const uint8_t* eocd = tail.data() + eocd_in_tail;
    uint64_t entry_count = read_u16(eocd + 10);
    uint64_t central_size = read_u32(eocd + 12);
    uint64_t central_offset = read_u32(eocd + 16);
    const uint16_t disk = read_u16(eocd + 4);
    const uint16_t central_disk = read_u16(eocd + 6);
    const uint16_t entries_on_disk = read_u16(eocd + 8);
    const uint64_t eocd_offset = file_size - search_size + eocd_in_tail;

    const bool zip64 =
        entry_count == 0xffffU || central_size == 0xffffffffU ||
        central_offset == 0xffffffffU;
    if(zip64) {
        if(eocd_offset < 20) {
            *error = "ZIP64 locator missing";
            return false;
        }
        std::array<uint8_t, 20> locator{};
        if(!read_at(in, eocd_offset - locator.size(), locator.data(), locator.size()) ||
           read_u32(locator.data()) != kZip64LocatorSignature ||
           read_u32(locator.data() + 4) != 0 ||
           read_u32(locator.data() + 16) != 1) {
            *error = "invalid ZIP64 locator";
            return false;
        }
        const uint64_t zip64_offset = read_u64(locator.data() + 8);
        std::array<uint8_t, 56> zip64_eocd{};
        if(!read_at(in, zip64_offset, zip64_eocd.data(), zip64_eocd.size()) ||
           read_u32(zip64_eocd.data()) != kZip64EocdSignature ||
           read_u32(zip64_eocd.data() + 16) != 0 ||
           read_u32(zip64_eocd.data() + 20) != 0) {
            *error = "invalid ZIP64 end record";
            return false;
        }
        entry_count = read_u64(zip64_eocd.data() + 32);
        central_size = read_u64(zip64_eocd.data() + 40);
        central_offset = read_u64(zip64_eocd.data() + 48);
    } else if(disk != 0 || central_disk != 0 || entries_on_disk != entry_count) {
        *error = "multi-disk ZIP is not supported";
        return false;
    }

    if(entry_count == 0 || entry_count > kMaxArchiveEntries ||
       central_offset > file_size || central_size > file_size - central_offset) {
        *error = "invalid ZIP central directory bounds";
        return false;
    }

    entries->clear();
    entries->reserve(static_cast<size_t>(entry_count));
    uint64_t pos = central_offset;
    const uint64_t central_end = central_offset + central_size;
    for(uint64_t index = 0; index < entry_count; ++index) {
        std::array<uint8_t, 46> header{};
        if(pos > central_end || central_end - pos < header.size() ||
           !read_at(in, pos, header.data(), header.size()) ||
           read_u32(header.data()) != kCentralHeaderSignature) {
            *error = "invalid ZIP central directory entry";
            return false;
        }

        const uint16_t name_len = read_u16(header.data() + 28);
        const uint16_t extra_len = read_u16(header.data() + 30);
        const uint16_t comment_len = read_u16(header.data() + 32);
        const uint64_t record_size =
            header.size() + static_cast<uint64_t>(name_len) + extra_len + comment_len;
        if(record_size > central_end - pos) {
            *error = "truncated ZIP central directory entry";
            return false;
        }

        std::vector<uint8_t> variable(static_cast<size_t>(name_len) + extra_len);
        if(!variable.empty() &&
           !read_at(in, pos + header.size(), variable.data(), variable.size())) {
            *error = "cannot read ZIP entry name";
            return false;
        }

        ZipEntry entry;
        entry.name.assign(reinterpret_cast<const char*>(variable.data()), name_len);
        entry.flags = read_u16(header.data() + 8);
        entry.method = read_u16(header.data() + 10);
        entry.crc32 = read_u32(header.data() + 16);
        entry.compressed_size = read_u32(header.data() + 20);
        entry.uncompressed_size = read_u32(header.data() + 24);
        entry.external_attributes = read_u32(header.data() + 38);
        entry.local_header_offset = read_u32(header.data() + 42);

        const bool need_uncompressed = entry.uncompressed_size == 0xffffffffU;
        const bool need_compressed = entry.compressed_size == 0xffffffffU;
        const bool need_offset = entry.local_header_offset == 0xffffffffU;
        if((need_uncompressed || need_compressed || need_offset) &&
           !parse_zip64_extra(
               std::vector<uint8_t>(variable.begin() + name_len, variable.end()),
               need_uncompressed, need_compressed, need_offset,
               &entry.uncompressed_size, &entry.compressed_size,
               &entry.local_header_offset)) {
            *error = "invalid ZIP64 entry metadata";
            return false;
        }

        if(!archive_entry_path_safe(entry.name)) {
            *error = "unsafe ZIP entry path";
            return false;
        }
        entries->push_back(std::move(entry));
        pos += record_size;
    }
    return true;
}

bool parse_json_u32_field(const std::string& text,
                          const char* key,
                          uint32_t* out,
                          bool* found)
{
    *found = false;
    const std::string needle = std::string("\"") + key + "\"";
    const size_t key_pos = text.find(needle);
    if(key_pos == std::string::npos) {
        return true;
    }
    size_t i = key_pos + needle.size();
    while(i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if(i >= text.size() || text[i] != ':') {
        return false;
    }
    ++i;
    while(i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if(i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) {
        return false;
    }
    uint64_t value = 0;
    while(i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        value = value * 10ULL + static_cast<uint64_t>(text[i] - '0');
        if(value > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        ++i;
    }
    *out = static_cast<uint32_t>(value);
    *found = true;
    return true;
}

/**
 * 解析 snowpath_plate_meta_v1：必填 layers>0 → layer_count；
 * 可选 print_time_s → duration_sec。
 */
bool parse_plate_meta_json(const std::string& path,
                           uint32_t* layer_count,
                           uint32_t* duration_sec,
                           std::string* error)
{
    *layer_count = 0;
    *duration_sec = 0;
    std::ifstream in(path);
    if(!in) {
        *error = "cannot open Metadata/*.json";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if(text.empty()) {
        *error = "Metadata/*.json is empty";
        return false;
    }

    bool found_layers = false;
    bool found_print_time = false;
    if(!parse_json_u32_field(text, "layers", layer_count, &found_layers) ||
       !parse_json_u32_field(text, "print_time_s", duration_sec, &found_print_time)) {
        *error = "Metadata/*.json has invalid layers or print_time_s";
        return false;
    }
    if(!found_layers || *layer_count == 0) {
        *error = "Metadata/*.json must provide layers > 0";
        return false;
    }
    (void)found_print_time;
    return true;
}

bool extract_entry(std::ifstream& in,
                   uint64_t archive_size,
                   const ZipEntry& entry,
                   const std::string& output_path,
                   uint64_t max_output,
                   std::string* error)
{
    if((entry.flags & 0x0001U) != 0) {
        *error = "encrypted ZIP entry is not supported";
        return false;
    }
    if(entry.method != 0 && entry.method != 8) {
        *error = "unsupported ZIP entry compression method";
        return false;
    }
    if(entry.uncompressed_size == 0 || entry.uncompressed_size > max_output) {
        *error = "ZIP entry is empty or too large";
        return false;
    }
    if(entry.compressed_size == 0 ||
       entry.uncompressed_size / entry.compressed_size > kMaxCompressionRatio) {
        *error = "ZIP entry compression ratio exceeds safety limit";
        return false;
    }

    /* Unix mode 的文件类型位为 0120000 时是符号链接。 */
    const uint32_t unix_mode = entry.external_attributes >> 16;
    if((unix_mode & 0170000U) == 0120000U) {
        *error = "symbolic-link ZIP entry is not allowed";
        return false;
    }

    std::array<uint8_t, 30> local{};
    if(!read_at(in, entry.local_header_offset, local.data(), local.size()) ||
       read_u32(local.data()) != kLocalHeaderSignature) {
        *error = "invalid local ZIP header";
        return false;
    }
    const uint16_t local_name_len = read_u16(local.data() + 26);
    const uint16_t local_extra_len = read_u16(local.data() + 28);
    if(entry.local_header_offset > archive_size ||
       local.size() > archive_size - entry.local_header_offset ||
       static_cast<uint64_t>(local_name_len) + local_extra_len >
           archive_size - entry.local_header_offset - local.size()) {
        *error = "local ZIP header is out of bounds";
        return false;
    }
    const uint64_t data_offset =
        entry.local_header_offset + local.size() + local_name_len + local_extra_len;
    if(data_offset > archive_size || entry.compressed_size > archive_size - data_offset) {
        *error = "G-code ZIP data is out of bounds";
        return false;
    }

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if(!out) {
        *error = "cannot create temporary ZIP entry";
        return false;
    }

    std::array<uint8_t, 65536> input{};
    std::array<uint8_t, 65536> output{};
    uint64_t compressed_left = entry.compressed_size;
    uint64_t produced = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    bool ok = true;

    if(entry.method == 0) {
        uint64_t offset = data_offset;
        while(compressed_left > 0 && ok) {
            const size_t count = static_cast<size_t>(
                std::min<uint64_t>(compressed_left, input.size()));
            if(!read_at(in, offset, input.data(), count)) {
                ok = false;
                break;
            }
            out.write(reinterpret_cast<const char*>(input.data()),
                      static_cast<std::streamsize>(count));
            ok = static_cast<bool>(out);
            crc = crc32(crc, input.data(), static_cast<uInt>(count));
            produced += count;
            compressed_left -= count;
            offset += count;
        }
    } else {
        z_stream stream{};
        if(inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
            *error = "cannot initialize ZIP inflater";
            out.close();
            std::remove(output_path.c_str());
            return false;
        }

        uint64_t offset = data_offset;
        int zret = Z_OK;
        while(zret != Z_STREAM_END && ok) {
            if(stream.avail_in == 0 && compressed_left > 0) {
                const size_t count = static_cast<size_t>(
                    std::min<uint64_t>(compressed_left, input.size()));
                if(!read_at(in, offset, input.data(), count)) {
                    ok = false;
                    break;
                }
                stream.next_in = input.data();
                stream.avail_in = static_cast<uInt>(count);
                compressed_left -= count;
                offset += count;
            }

            stream.next_out = output.data();
            stream.avail_out = static_cast<uInt>(output.size());
            zret = inflate(&stream, Z_NO_FLUSH);
            if(zret != Z_OK && zret != Z_STREAM_END) {
                ok = false;
                break;
            }
            const size_t count = output.size() - stream.avail_out;
            if(count > max_output - std::min(produced, max_output)) {
                ok = false;
                break;
            }
            if(count > 0) {
                out.write(reinterpret_cast<const char*>(output.data()),
                          static_cast<std::streamsize>(count));
                ok = static_cast<bool>(out);
                crc = crc32(crc, output.data(), static_cast<uInt>(count));
                produced += count;
            }
            if(stream.avail_in == 0 && compressed_left == 0 &&
               zret != Z_STREAM_END && count == 0) {
                ok = false;
                break;
            }
        }
        const uint64_t consumed = stream.total_in;
        inflateEnd(&stream);
        ok = ok && zret == Z_STREAM_END && compressed_left == 0 &&
             consumed == entry.compressed_size;
    }

    out.close();
    ok = ok && static_cast<bool>(out) &&
         produced == entry.uncompressed_size &&
         static_cast<uint32_t>(crc) == entry.crc32;
    if(!ok) {
        std::remove(output_path.c_str());
        *error = "ZIP entry decompression or CRC verification failed";
        return false;
    }
    return true;
}

std::string output_gcode_name(const std::string& package_name)
{
    std::string base = std::filesystem::path(package_name).filename().string();
    const std::string lower = lower_ascii(base);
    if(ends_with(lower, ".3mf")) {
        base.resize(base.size() - 4);
    }
    if(base.empty()) {
        base = "transfer";
    }
    for(char& c : base) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if(c == '/' || c == '\\' || uc < 0x20 || c == ':') {
            c = '_';
        }
    }
    return base + ".gcode";
}

const ZipEntry* select_named_sidecar_entry(
    const std::vector<ZipEntry>& entries,
    const ZipEntry& gcode_entry,
    bool (*matcher)(const std::string&),
    const char* kind,
    std::string* reason)
{
    std::vector<const ZipEntry*> matched_kind;
    for(const ZipEntry& entry : entries) {
        if(matcher(entry.name)) {
            matched_kind.push_back(&entry);
        }
    }
    if(matched_kind.empty()) {
        *reason = std::string("package has no ") + kind + " entry";
        return nullptr;
    }
    if(matched_kind.size() == 1) {
        return matched_kind.front();
    }

    const std::string wanted =
        lower_ascii(std::filesystem::path(gcode_entry.name).stem().string());
    const ZipEntry* matched = nullptr;
    for(const ZipEntry* entry : matched_kind) {
        if(lower_ascii(std::filesystem::path(entry->name).stem().string()) != wanted) {
            continue;
        }
        if(matched) {
            *reason = std::string("multiple ") + kind +
                      " entries match the G-code name";
            return nullptr;
        }
        matched = entry;
    }
    if(!matched) {
        *reason = std::string("multiple ") + kind +
                  " entries and none uniquely matches the G-code name";
    }
    return matched;
}

const ZipEntry* select_thumbnail_entry(const std::vector<ZipEntry>& entries,
                                       const ZipEntry& gcode_entry,
                                       std::string* reason)
{
    return select_named_sidecar_entry(
        entries, gcode_entry, is_thumbnail_png_entry, "Thumbnails/*.png", reason);
}

const ZipEntry* select_metadata_entry(const std::vector<ZipEntry>& entries,
                                      const ZipEntry& gcode_entry,
                                      std::string* reason)
{
    return select_named_sidecar_entry(
        entries, gcode_entry, is_metadata_json_entry, "Metadata/*.json", reason);
}

std::filesystem::path importing_path(const std::filesystem::path& final_path)
{
    return final_path.parent_path() /
        ("." + final_path.filename().string() + ".importing");
}

std::filesystem::path backup_path(const std::filesystem::path& final_path)
{
    return final_path.parent_path() /
        ("." + final_path.filename().string() + ".backup");
}

struct StagedAsset {
    std::filesystem::path staged;
    std::filesystem::path final;
    std::filesystem::path backup;
    bool had_previous = false;
    bool previous_moved = false;
    bool installed = false;
};

void rollback_assets(std::vector<StagedAsset>* assets)
{
    std::error_code ec;
    for(auto it = assets->rbegin(); it != assets->rend(); ++it) {
        if(it->installed) {
            std::filesystem::remove(it->final, ec);
            ec.clear();
        }
        if(it->previous_moved) {
            (void)std::rename(it->backup.string().c_str(), it->final.string().c_str());
        }
        std::filesystem::remove(it->staged, ec);
        ec.clear();
    }
}

bool install_assets(std::vector<StagedAsset>* assets, std::string* error)
{
    std::error_code ec;
    for(StagedAsset& asset : *assets) {
        std::filesystem::remove(asset.backup, ec);
        ec.clear();
        asset.had_previous = std::filesystem::exists(asset.final, ec) && !ec;
        if(ec) {
            *error = "cannot inspect previous imported asset";
            rollback_assets(assets);
            return false;
        }
        if(asset.had_previous) {
            if(!std::filesystem::is_regular_file(asset.final, ec) || ec ||
               std::rename(asset.final.string().c_str(), asset.backup.string().c_str()) != 0) {
                *error = "cannot stage previous imported asset";
                rollback_assets(assets);
                return false;
            }
            asset.previous_moved = true;
        }
        if(std::rename(asset.staged.string().c_str(), asset.final.string().c_str()) != 0) {
            *error = "cannot install imported asset";
            rollback_assets(assets);
            return false;
        }
        asset.installed = true;
    }
    return true;
}

void finish_assets(std::vector<StagedAsset>* assets)
{
    std::error_code ec;
    for(StagedAsset& asset : *assets) {
        std::filesystem::remove(asset.backup, ec);
        ec.clear();
    }
}

void remove_local_thumbnail_file(const std::string& raw_path)
{
    if(raw_path.empty()) {
        return;
    }
    std::string path = raw_path;
    if(path.compare(0, 3, "A:/") == 0) {
        path.erase(0, 2);
    }
    const std::filesystem::path root =
        std::filesystem::path(UPC_LOCAL_GCODE_THUMB_DIR).lexically_normal();
    const std::filesystem::path normalized =
        std::filesystem::path(path).lexically_normal();
    if(normalized.parent_path() != root) {
        return;
    }
    std::error_code ec;
    if(std::filesystem::is_regular_file(normalized, ec) && !ec) {
        std::filesystem::remove(normalized, ec);
    }
}

Transfer3mfImportResult fail(int status,
                             const std::string& code,
                             const std::string& message)
{
    Transfer3mfImportResult result;
    result.http_status = status;
    result.err_code = code;
    result.err_msg = message;
    return result;
}

} // namespace

Transfer3mfImporter::Transfer3mfImporter(std::string gcode_dir, uint64_t max_gcode_bytes)
    : gcode_dir_(std::move(gcode_dir)),
      max_gcode_bytes_(max_gcode_bytes)
{
}

Transfer3mfImportResult Transfer3mfImporter::import_file(
    const std::string& package_path,
    const std::string& package_name) const
{
    std::lock_guard<std::mutex> import_lock(import_mu_);

    std::error_code ec;
    const uint64_t archive_size = std::filesystem::file_size(package_path, ec);
    if(ec || archive_size == 0) {
        return fail(500, "3MF_IO_ERROR", "cannot read uploaded 3mf");
    }

    std::ifstream archive(package_path, std::ios::binary);
    if(!archive) {
        return fail(500, "3MF_IO_ERROR", "cannot open uploaded 3mf");
    }

    std::vector<ZipEntry> entries;
    std::string detail;
    if(!read_zip_directory(archive, archive_size, &entries, &detail)) {
        return fail(422, "3MF_INVALID_ARCHIVE", detail);
    }

    std::vector<const ZipEntry*> gcode_entries;
    for(const ZipEntry& entry : entries) {
        if(is_gcode_entry(entry.name)) {
            gcode_entries.push_back(&entry);
        }
    }
    if(gcode_entries.empty()) {
        return fail(422, "3MF_GCODE_NOT_FOUND",
                    "3mf must contain one 3D/*.gcode or Metadata/*.gcode entry");
    }
    if(gcode_entries.size() != 1) {
        return fail(422, "3MF_MULTIPLE_GCODE",
                    "3mf contains multiple G-code entries; only one is supported");
    }

    std::filesystem::create_directories(gcode_dir_, ec);
    if(ec) {
        return fail(500, "3MF_IO_ERROR", "cannot create G-code directory");
    }

    const std::string printable_name = output_gcode_name(package_name);
    const std::filesystem::path gcode_final =
        std::filesystem::path(gcode_dir_) / printable_name;
    const std::filesystem::path gcode_staged = importing_path(gcode_final);
    std::filesystem::remove(gcode_staged, ec);

    if(!extract_entry(archive, archive_size, *gcode_entries.front(),
                      gcode_staged.string(), max_gcode_bytes_, &detail)) {
        return fail(422, "3MF_GCODE_EXTRACT_FAILED", detail);
    }

    const uint64_t gcode_size = std::filesystem::file_size(gcode_staged, ec);
    if(ec || gcode_size == 0 || gcode_size > std::numeric_limits<uint32_t>::max()) {
        std::filesystem::remove(gcode_staged, ec);
        return fail(422, "3MF_GCODE_INVALID",
                    "extracted G-code is empty or size is unsupported");
    }

    std::string meta_reason;
    const ZipEntry* metadata_entry =
        select_metadata_entry(entries, *gcode_entries.front(), &meta_reason);
    if(!metadata_entry) {
        std::filesystem::remove(gcode_staged, ec);
        return fail(422, "3MF_META_NOT_FOUND", meta_reason);
    }

    const std::filesystem::path meta_staged =
        importing_path(std::filesystem::path(gcode_dir_) /
                       (std::filesystem::path(printable_name).stem().string() +
                        ".meta.json"));
    std::filesystem::remove(meta_staged, ec);
    if(!extract_entry(archive, archive_size, *metadata_entry,
                      meta_staged.string(), kMaxMetadataJsonBytes, &detail)) {
        std::filesystem::remove(gcode_staged, ec);
        std::filesystem::remove(meta_staged, ec);
        return fail(422, "3MF_META_EXTRACT_FAILED", detail);
    }

    uint32_t layer_count = 0;
    uint32_t duration_sec = 0;
    if(!parse_plate_meta_json(meta_staged.string(), &layer_count, &duration_sec,
                              &detail)) {
        std::filesystem::remove(gcode_staged, ec);
        std::filesystem::remove(meta_staged, ec);
        return fail(422, "3MF_META_INVALID", detail);
    }
    std::filesystem::remove(meta_staged, ec);

    const std::string asset_stem =
        std::filesystem::path(printable_name).stem().string();
    const std::filesystem::path thumb_root = UPC_LOCAL_GCODE_THUMB_DIR;
    const std::filesystem::path png_final = thumb_root / (asset_stem + ".png");
    const std::filesystem::path small_final = thumb_root / (asset_stem + "_small.rthm");
    const std::filesystem::path large_final = thumb_root / (asset_stem + "_large.rthm");
    const std::filesystem::path print_final = thumb_root / (asset_stem + "_print.rthm");
    const std::filesystem::path png_staged = importing_path(png_final);
    const std::array<TransferThumbnailTarget, 3> thumbnail_targets{{
        {importing_path(small_final).string(), 72, 72},
        {importing_path(large_final).string(), 320, 180},
        {importing_path(print_final).string(), 100, 100},
    }};

    bool thumbnails_ready = false;
    std::string thumbnail_reason;
    const ZipEntry* thumbnail_entry =
        select_thumbnail_entry(entries, *gcode_entries.front(), &thumbnail_reason);
    if(thumbnail_entry) {
        std::filesystem::create_directories(thumb_root, ec);
        if(ec) {
            thumbnail_reason = "cannot create local thumbnail directory";
        } else {
            std::filesystem::remove(png_staged, ec);
            for(const TransferThumbnailTarget& target : thumbnail_targets) {
                std::filesystem::remove(target.path, ec);
            }
            if(!extract_entry(archive, archive_size, *thumbnail_entry,
                              png_staged.string(), kMaxThumbnailPngBytes,
                              &thumbnail_reason)) {
                std::filesystem::remove(png_staged, ec);
            } else if(!generate_transfer_thumbnail_triplet(
                          png_staged.string(), thumbnail_targets, &thumbnail_reason)) {
                std::filesystem::remove(png_staged, ec);
            } else {
                /* PNG 只作解码缓存，RTHM 生成后立即删除。 */
                std::filesystem::remove(png_staged, ec);
                thumbnails_ready = true;
            }
        }
    }
    if(!thumbnails_ready) {
        UPC_LOG_XFR_SERVICE(Warn,
            "3mf thumbnail skipped package=%s reason=%s",
            package_name.c_str(), thumbnail_reason.c_str());
        std::filesystem::remove(png_staged, ec);
        for(const TransferThumbnailTarget& target : thumbnail_targets) {
            std::filesystem::remove(target.path, ec);
        }
    }

    std::vector<GcodeManifestEntry> manifest;
    if(!gcode_manifest_load(UPC_LOCAL_GCODE_CONFIG, manifest)) {
        std::filesystem::remove(gcode_staged, ec);
        std::filesystem::remove(png_staged, ec);
        for(const TransferThumbnailTarget& target : thumbnail_targets) {
            std::filesystem::remove(target.path, ec);
        }
        return fail(500, "3MF_MANIFEST_ERROR", "cannot load G-code manifest");
    }

    GcodeManifestEntry item;
    item.name = printable_name;
    item.file_size = static_cast<uint32_t>(gcode_size);
    item.line_count = 0; /* 打印走纯层进度，不在导入阶段统计行数 */
    item.layer_count = layer_count;
    item.duration_sec = duration_sec;
    item.material_g = 0.f;
    item.mtime = static_cast<int64_t>(std::time(nullptr));
    if(thumbnails_ready) {
        item.thumb_small = small_final.string();
        item.thumb_large = large_final.string();
        item.thumb_print = print_final.string();
    }

    std::array<std::string, 3> old_thumbnail_paths{};
    bool replaced = false;
    for(GcodeManifestEntry& existing : manifest) {
        if(existing.name != printable_name) {
            continue;
        }
        old_thumbnail_paths = {
            existing.thumb_small, existing.thumb_large, existing.thumb_print};
        existing = item;
        replaced = true;
        break;
    }
    if(!replaced) {
        manifest.push_back(item);
    }

    const std::filesystem::path staged_manifest =
        std::filesystem::path(UPC_LOCAL_GCODE_CONFIG).string() + ".importing";
    std::filesystem::remove(staged_manifest, ec);
    if(!gcode_manifest_save(staged_manifest.string(), manifest)) {
        std::filesystem::remove(gcode_staged, ec);
        std::filesystem::remove(png_staged, ec);
        for(const TransferThumbnailTarget& target : thumbnail_targets) {
            std::filesystem::remove(target.path, ec);
        }
        return fail(500, "3MF_MANIFEST_ERROR", "cannot stage G-code manifest");
    }

    std::vector<StagedAsset> assets;
    assets.push_back(
        {gcode_staged, gcode_final, backup_path(gcode_final)});
    if(thumbnails_ready) {
        assets.push_back({
            thumbnail_targets[0].path, small_final, backup_path(small_final)});
        assets.push_back({
            thumbnail_targets[1].path, large_final, backup_path(large_final)});
        assets.push_back({
            thumbnail_targets[2].path, print_final, backup_path(print_final)});
    }
    if(!install_assets(&assets, &detail)) {
        std::filesystem::remove(staged_manifest, ec);
        return fail(500, "3MF_IO_ERROR", detail);
    }
    if(std::rename(staged_manifest.string().c_str(), UPC_LOCAL_GCODE_CONFIG) != 0) {
        rollback_assets(&assets);
        std::filesystem::remove(staged_manifest, ec);
        return fail(500, "3MF_MANIFEST_ERROR", "cannot install G-code manifest");
    }
    finish_assets(&assets);

    const std::array<std::string, 3> current_thumbnail_paths{{
        item.thumb_small, item.thumb_large, item.thumb_print}};
    for(size_t i = 0; i < old_thumbnail_paths.size(); ++i) {
        if(old_thumbnail_paths[i] != current_thumbnail_paths[i]) {
            remove_local_thumbnail_file(old_thumbnail_paths[i]);
        }
    }
    /* 无论本次是否出图，清掉历史残留 PNG（仅作过解码缓存）。 */
    remove_local_thumbnail_file(png_final.string());
    if(!thumbnails_ready) {
        remove_local_thumbnail_file(small_final.string());
        remove_local_thumbnail_file(large_final.string());
        remove_local_thumbnail_file(print_final.string());
    }

    UPC_LOG_XFR_SERVICE(Info,
        "3mf imported package=%s entry=%s output=%s bytes=%llu layers=%u "
        "duration_s=%u thumbnail=%s",
        package_name.c_str(), gcode_entries.front()->name.c_str(),
        gcode_final.string().c_str(),
        static_cast<unsigned long long>(gcode_size),
        static_cast<unsigned>(layer_count),
        static_cast<unsigned>(duration_sec),
        thumbnails_ready ? small_final.string().c_str() : "none");

    Transfer3mfImportResult result;
    result.ok = true;
    result.http_status = 200;
    result.printable_name = printable_name;
    result.printable_size = gcode_size;
    result.line_count = 0;
    result.layer_count = layer_count;
    return result;
}

} // namespace upc::xfr
