#include "transfer_3mf_importer.hpp"
#include "gcode_manifest.hpp"
#include "raw_thumb_format.hpp"

#include <zlib.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct FixtureEntry {
    std::string name;
    std::string data;
};

void put_u16(std::ofstream& out, uint16_t value)
{
    const std::array<uint8_t, 2> bytes{{
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)}};
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void put_u32(std::ofstream& out, uint32_t value)
{
    const std::array<uint8_t, 4> bytes{{
        static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)}};
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool write_fixture_zip(const std::filesystem::path& path,
                       const std::vector<FixtureEntry>& entries)
{
    struct Central {
        FixtureEntry entry;
        uint32_t crc = 0;
        uint32_t offset = 0;
    };
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if(!out) {
        return false;
    }
    std::vector<Central> central;
    for(const FixtureEntry& entry : entries) {
        Central item;
        item.entry = entry;
        item.offset = static_cast<uint32_t>(out.tellp());
        item.crc = static_cast<uint32_t>(crc32(
            0L, reinterpret_cast<const Bytef*>(entry.data.data()),
            static_cast<uInt>(entry.data.size())));
        put_u32(out, 0x04034b50U);
        put_u16(out, 20);
        put_u16(out, 0);
        put_u16(out, 0); /* stored */
        put_u16(out, 0);
        put_u16(out, 0);
        put_u32(out, item.crc);
        put_u32(out, static_cast<uint32_t>(entry.data.size()));
        put_u32(out, static_cast<uint32_t>(entry.data.size()));
        put_u16(out, static_cast<uint16_t>(entry.name.size()));
        put_u16(out, 0);
        out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
        out.write(entry.data.data(), static_cast<std::streamsize>(entry.data.size()));
        central.push_back(std::move(item));
    }

    const uint32_t central_offset = static_cast<uint32_t>(out.tellp());
    for(const Central& item : central) {
        put_u32(out, 0x02014b50U);
        put_u16(out, 0x0314);
        put_u16(out, 20);
        put_u16(out, 0);
        put_u16(out, 0);
        put_u16(out, 0);
        put_u16(out, 0);
        put_u32(out, item.crc);
        put_u32(out, static_cast<uint32_t>(item.entry.data.size()));
        put_u32(out, static_cast<uint32_t>(item.entry.data.size()));
        put_u16(out, static_cast<uint16_t>(item.entry.name.size()));
        put_u16(out, 0);
        put_u16(out, 0);
        put_u16(out, 0);
        put_u16(out, 0);
        put_u32(out, 0100644U << 16);
        put_u32(out, item.offset);
        out.write(item.entry.name.data(),
                  static_cast<std::streamsize>(item.entry.name.size()));
    }
    const uint32_t central_size =
        static_cast<uint32_t>(out.tellp()) - central_offset;
    put_u32(out, 0x06054b50U);
    put_u16(out, 0);
    put_u16(out, 0);
    put_u16(out, static_cast<uint16_t>(central.size()));
    put_u16(out, static_cast<uint16_t>(central.size()));
    put_u32(out, central_size);
    put_u32(out, central_offset);
    put_u16(out, 0);
    return static_cast<bool>(out);
}

bool check_rthm(const std::string& path, uint32_t width, uint32_t height)
{
    std::ifstream in(path, std::ios::binary);
    upc::RawThumbHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path, ec);
    return in && !ec && upc::raw_thumb_header_valid(header) &&
           header.width == width && header.height == height &&
           header.stride_bytes == width * 2u &&
           header.data_size == width * height * 2u &&
           size == sizeof(header) + header.data_size;
}

const upc::GcodeManifestEntry* find_entry(
    const std::vector<upc::GcodeManifestEntry>& entries,
    const std::string& name)
{
    for(const auto& entry : entries) {
        if(entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

std::string plate_meta_json(uint32_t layers, uint32_t print_time_s)
{
    return std::string("{\"schema\":\"snowpath_plate_meta_v1\",\"layers\":") +
           std::to_string(layers) + ",\"print_time_s\":" +
           std::to_string(print_time_s) + "}";
}

} // namespace

int main(int argc, char** argv)
{
    if(argc != 3) {
        std::cerr << "usage: transfer_3mf_smoke <sample.3mf> <layers>\n";
        return 2;
    }

    const std::filesystem::path output_dir = UPC_LOCAL_GCODE_DIR;
    const std::filesystem::path thumb_dir = UPC_LOCAL_GCODE_THUMB_DIR;
    std::error_code ec;
    std::filesystem::remove_all(output_dir, ec);
    std::filesystem::create_directories(output_dir, ec);
    if(ec) {
        std::cerr << "cannot create test output: " << ec.message() << "\n";
        return 2;
    }

    const uint32_t expected_layers =
        static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));

    upc::xfr::Transfer3mfImporter importer(output_dir.string());
    const auto result = importer.import_file(argv[1], "竖放_0_transfer.3mf");
    if(!result.ok) {
        std::cerr << result.err_code << ": " << result.err_msg << "\n";
        return 1;
    }

    const std::filesystem::path output = output_dir / result.printable_name;
    std::vector<upc::GcodeManifestEntry> manifest;
    const bool manifest_ok =
        upc::gcode_manifest_load(UPC_LOCAL_GCODE_CONFIG, manifest);
    const upc::GcodeManifestEntry* item =
        find_entry(manifest, result.printable_name);
    const bool ok =
        std::filesystem::is_regular_file(output) &&
        std::filesystem::is_regular_file(UPC_LOCAL_GCODE_CONFIG) &&
        manifest_ok && item &&
        !std::filesystem::exists(thumb_dir / "竖放_0_transfer.png") &&
        check_rthm(item->thumb_small, 72, 72) &&
        check_rthm(item->thumb_large, 320, 180) &&
        check_rthm(item->thumb_print, 100, 100) &&
        result.line_count == 0 &&
        item->line_count == 0 &&
        result.layer_count == expected_layers &&
        item->layer_count == expected_layers;
    std::cout << "name=" << result.printable_name
              << " bytes=" << result.printable_size
              << " lines=" << result.line_count
              << " layers=" << result.layer_count
              << " duration_s=" << (item ? item->duration_sec : 0) << "\n";
    if(!ok) {
        std::cerr << "import result does not match expected files/statistics\n";
        return 1;
    }

    /* 同名重传的 PNG 损坏时，G-code 仍成功替换，但旧图片和清单路径必须清空。 */
    const std::filesystem::path bad_zip =
        std::filesystem::path(UPC_LOCAL_GCODE_DIR).parent_path() / "bad-thumb.3mf";
    if(!write_fixture_zip(bad_zip, {
            {"3D/竖放_0.gcode", "; replacement\nM3000\nG1 X1\n"},
            {"Metadata/竖放_0.json", plate_meta_json(2, 60)},
            {"Thumbnails/竖放_0.png", "not-a-png"},
        })) {
        std::cerr << "cannot create corrupt-thumbnail fixture\n";
        return 1;
    }
    const auto replaced = importer.import_file(bad_zip.string(), "竖放_0_transfer.3mf");
    manifest.clear();
    if(!replaced.ok ||
       !upc::gcode_manifest_load(UPC_LOCAL_GCODE_CONFIG, manifest)) {
        std::cerr << "corrupt thumbnail incorrectly rejected G-code import\n";
        return 1;
    }
    item = find_entry(manifest, replaced.printable_name);
    if(!item || item->layer_count != 2 || item->duration_sec != 60 ||
       !item->thumb_small.empty() || !item->thumb_large.empty() ||
       !item->thumb_print.empty() ||
       std::filesystem::exists(thumb_dir / "竖放_0_transfer.png") ||
       std::filesystem::exists(thumb_dir / "竖放_0_transfer_small.rthm") ||
       std::filesystem::exists(thumb_dir / "竖放_0_transfer_large.rthm") ||
       std::filesystem::exists(thumb_dir / "竖放_0_transfer_print.rthm")) {
        std::cerr << "corrupt thumbnail left stale manifest paths or image assets\n";
        return 1;
    }

    /* 缺少 Thumbnails 目录同样只降级为空缩略图，不影响新 G-code 导入。 */
    const std::filesystem::path no_thumb_zip =
        std::filesystem::path(UPC_LOCAL_GCODE_DIR).parent_path() / "no-thumb.3mf";
    if(!write_fixture_zip(no_thumb_zip, {
            {"3D/no-thumb.gcode", "; no thumbnail\nM3000\n"},
            {"Metadata/no-thumb.json", plate_meta_json(3, 0)},
        })) {
        std::cerr << "cannot create no-thumbnail fixture\n";
        return 1;
    }
    const auto no_thumb =
        importer.import_file(no_thumb_zip.string(), "missing_transfer.3mf");
    manifest.clear();
    if(!no_thumb.ok ||
       !upc::gcode_manifest_load(UPC_LOCAL_GCODE_CONFIG, manifest)) {
        std::cerr << "missing thumbnail incorrectly rejected G-code import\n";
        return 1;
    }
    item = find_entry(manifest, no_thumb.printable_name);
    if(!item || item->layer_count != 3 || !item->thumb_small.empty() ||
       !item->thumb_large.empty() || !item->thumb_print.empty()) {
        std::cerr << "missing thumbnail did not leave empty manifest paths\n";
        return 1;
    }

    /* 缺少 Metadata/*.json 必须拒绝导入。 */
    const std::filesystem::path no_meta_zip =
        std::filesystem::path(UPC_LOCAL_GCODE_DIR).parent_path() / "no-meta.3mf";
    if(!write_fixture_zip(no_meta_zip, {
            {"3D/no-meta.gcode", "; no meta\nM3000\n"},
        })) {
        std::cerr << "cannot create no-metadata fixture\n";
        return 1;
    }
    const auto no_meta =
        importer.import_file(no_meta_zip.string(), "no_meta_transfer.3mf");
    if(no_meta.ok || no_meta.err_code != "3MF_META_NOT_FOUND") {
        std::cerr << "missing Metadata/*.json should reject import\n";
        return 1;
    }

    std::cout << "thumbnail triplet, optional-thumbnail and metadata cases passed\n";
    return 0;
}
