#include "ovf_official_io.h"

#include "ovf_lut.pb.h"

#include <google/protobuf/util/delimited_message_util.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace open_vector_format {
namespace {

namespace fs = std::filesystem;

constexpr std::array<std::uint8_t, 4> kMagicBytes{{0x4c, 0x56, 0x46, 0x21}};  // "LVF!"

/// path is UTF-8 (e.g. Qt QString::toStdString / toUtf8). On Windows, narrow
/// ofstream would mis-handle non-ASCII；C++17 用 u8path，C++20+ 可用 u8string。
fs::path pathFromUtf8(const std::string& utf8)
{
#if defined(__cpp_lib_char8_t) && __cpp_lib_char8_t >= 201811L
    std::u8string u8(utf8.size(), char8_t{});
    if (!utf8.empty()) {
        std::memcpy(u8.data(), utf8.data(), utf8.size());
    }
    return fs::path(u8);
#else
    return fs::u8path(utf8);
#endif
}

void writeLittleEndianI64(std::ostream& os, std::int64_t value)
{
    const auto u = static_cast<std::uint64_t>(value);
    char buf[8];
    for (int i = 0; i < 8; ++i) {
        buf[i] = static_cast<char>((u >> (8 * i)) & 0xffu);
    }
    os.write(buf, 8);
}

bool writeDelimited(const google::protobuf::MessageLite& msg, std::ostream& os)
{
    return google::protobuf::util::SerializeDelimitedToOstream(msg, &os);
}

bool looksLikeOfficialOvf(const std::vector<char>& data)
{
    if (data.size() < 4) {
        return false;
    }
    return static_cast<std::uint8_t>(data[0]) == kMagicBytes[0]
        && static_cast<std::uint8_t>(data[1]) == kMagicBytes[1]
        && static_cast<std::uint8_t>(data[2]) == kMagicBytes[2]
        && static_cast<std::uint8_t>(data[3]) == kMagicBytes[3];
}

bool readFileBytes(const std::string& path, std::vector<char>& out, std::string* errorOut)
{
    std::ifstream ifs(pathFromUtf8(path), std::ios::binary);
    if (!ifs) {
        if (errorOut) {
            *errorOut = "failed to open input: " + path;
        }
        return false;
    }
    ifs.seekg(0, std::ios::end);
    const auto sz = ifs.tellg();
    if (sz < 0) {
        if (errorOut) {
            *errorOut = "failed to size input: " + path;
        }
        return false;
    }
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<std::size_t>(sz));
    if (!out.empty()) {
        ifs.read(out.data(), static_cast<std::streamsize>(out.size()));
        if (!ifs) {
            if (errorOut) {
                *errorOut = "failed to read input: " + path;
            }
            return false;
        }
    }
    return true;
}

bool readLittleEndianI64(std::istream& is, std::int64_t& value)
{
    char buf[8];
    is.read(buf, 8);
    if (!is) {
        return false;
    }
    std::uint64_t u = 0;
    for (int i = 0; i < 8; ++i) {
        u |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[i]))
            << (8 * i);
    }
    value = static_cast<std::int64_t>(u);
    return true;
}

/// Size-delimited protobuf (varint length + payload). Implemented locally:
/// this protobuf build has SerializeDelimitedToOstream but not
/// ParseDelimitedFromIstream; seek-based OVF I/O also needs exact reads
/// (no ZeroCopyInputStream read-ahead).
bool parseDelimited(google::protobuf::MessageLite& msg, std::istream& is)
{
    std::uint32_t size = 0;
    int shift = 0;
    for (;;) {
        char c = 0;
        is.read(&c, 1);
        if (!is) {
            return false;
        }
        const auto b = static_cast<std::uint8_t>(c);
        size |= static_cast<std::uint32_t>(b & 0x7fu) << shift;
        if ((b & 0x80u) == 0) {
            break;
        }
        shift += 7;
        if (shift > 28) {
            return false;
        }
    }

    msg.Clear();
    if (size == 0) {
        return true;
    }

    std::vector<char> buf(static_cast<std::size_t>(size));
    is.read(buf.data(), static_cast<std::streamsize>(size));
    if (!is) {
        return false;
    }
    return msg.ParseFromArray(buf.data(), static_cast<int>(size));
}

}  // namespace

bool writeOfficialOvfFile(const Job& job, const std::string& path, std::string* errorOut)
{
    // path is UTF-8; use fs::path so Windows opens via wchar (Chinese project names).
    std::ofstream ofs(pathFromUtf8(path), std::ios::binary | std::ios::trunc);
    if (!ofs) {
        if (errorOut) {
            *errorOut = "failed to create: " + path;
        }
        return false;
    }

    ofs.write(reinterpret_cast<const char*>(kMagicBytes.data()),
              static_cast<std::streamsize>(kMagicBytes.size()));

    const auto jobLutOffsetPos = ofs.tellp();
    writeLittleEndianI64(ofs, 0);

    Job jobShell;
    jobShell.CopyFrom(job);
    jobShell.clear_work_planes();
    jobShell.set_num_work_planes(0);

    JobLUT jobLut;

    const int nPlanes = job.work_planes_size();
    for (int i = 0; i < nPlanes; ++i) {
        const WorkPlane& wp = job.work_planes(i);

        const auto workplaneOffset = ofs.tellp();
        jobLut.add_workplanepositions(static_cast<std::int64_t>(workplaneOffset));
        writeLittleEndianI64(ofs, 0);  // placeholder → WorkPlaneLUT offset

        WorkPlaneLUT wpLut;
        for (int b = 0; b < wp.vector_blocks_size(); ++b) {
            wpLut.add_vectorblockspositions(static_cast<std::int64_t>(ofs.tellp()));
            if (!writeDelimited(wp.vector_blocks(b), ofs)) {
                if (errorOut) {
                    *errorOut = "failed to write vector block";
                }
                return false;
            }
        }

        WorkPlane shellToWrite;
        shellToWrite.CopyFrom(wp);
        shellToWrite.clear_vector_blocks();
        shellToWrite.set_work_plane_number(jobShell.num_work_planes());

        wpLut.set_workplaneshellposition(static_cast<std::int64_t>(ofs.tellp()));
        if (!writeDelimited(shellToWrite, ofs)) {
            if (errorOut) {
                *errorOut = "failed to write work plane shell";
            }
            return false;
        }

        const auto workplaneLutOffset = ofs.tellp();
        if (!writeDelimited(wpLut, ofs)) {
            if (errorOut) {
                *errorOut = "failed to write work plane LUT";
            }
            return false;
        }

        ofs.seekp(workplaneOffset);
        writeLittleEndianI64(ofs, static_cast<std::int64_t>(workplaneLutOffset));
        ofs.seekp(0, std::ios::end);

        jobShell.set_num_work_planes(jobShell.num_work_planes() + 1);
    }

    jobLut.set_jobshellposition(static_cast<std::int64_t>(ofs.tellp()));
    if (!writeDelimited(jobShell, ofs)) {
        if (errorOut) {
            *errorOut = "failed to write job shell";
        }
        return false;
    }

    const auto jobLutOffset = ofs.tellp();
    ofs.seekp(jobLutOffsetPos);
    writeLittleEndianI64(ofs, static_cast<std::int64_t>(jobLutOffset));
    ofs.seekp(0, std::ios::end);

    if (!writeDelimited(jobLut, ofs)) {
        if (errorOut) {
            *errorOut = "failed to write job LUT";
        }
        return false;
    }

    ofs.flush();
    if (!ofs) {
        if (errorOut) {
            *errorOut = "stream error while writing: " + path;
        }
        return false;
    }
    return true;
}

bool readOfficialOvfFile(const std::string& path, Job& jobOut, std::string* errorOut)
{
    std::ifstream ifs(pathFromUtf8(path), std::ios::binary);
    if (!ifs) {
        if (errorOut) {
            *errorOut = "failed to open input: " + path;
        }
        return false;
    }

    char magic[4] = {};
    ifs.read(magic, 4);
    if (!ifs) {
        if (errorOut) {
            *errorOut = "failed to read ovf header: " + path;
        }
        return false;
    }

    const bool official = static_cast<std::uint8_t>(magic[0]) == kMagicBytes[0]
        && static_cast<std::uint8_t>(magic[1]) == kMagicBytes[1]
        && static_cast<std::uint8_t>(magic[2]) == kMagicBytes[2]
        && static_cast<std::uint8_t>(magic[3]) == kMagicBytes[3];

    if (!official) {
        // Fallback: raw Job.SerializeToOstream dump (no LVF! header).
        ifs.clear();
        ifs.seekg(0, std::ios::beg);
        std::vector<char> data;
        ifs.seekg(0, std::ios::end);
        const auto sz = ifs.tellg();
        if (sz < 0) {
            if (errorOut) {
                *errorOut = "failed to size input: " + path;
            }
            return false;
        }
        ifs.seekg(0, std::ios::beg);
        data.resize(static_cast<std::size_t>(sz));
        if (!data.empty()) {
            ifs.read(data.data(), static_cast<std::streamsize>(data.size()));
            if (!ifs) {
                if (errorOut) {
                    *errorOut = "failed to read raw Job dump: " + path;
                }
                return false;
            }
        }
        jobOut.Clear();
        if (!jobOut.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
            if (errorOut) {
                *errorOut =
                    "input is neither official LVF! ovf nor a raw Job protobuf dump";
            }
            return false;
        }
        return true;
    }

    std::int64_t jobLutOffset = 0;
    if (!readLittleEndianI64(ifs, jobLutOffset) || jobLutOffset <= 0) {
        if (errorOut) {
            *errorOut = "invalid job LUT offset";
        }
        return false;
    }

    ifs.seekg(static_cast<std::streamoff>(jobLutOffset));
    JobLUT jobLut;
    if (!parseDelimited(jobLut, ifs)) {
        if (errorOut) {
            *errorOut = "failed to parse JobLUT";
        }
        return false;
    }

    jobOut.Clear();
    ifs.seekg(static_cast<std::streamoff>(jobLut.jobshellposition()));
    if (!parseDelimited(jobOut, ifs)) {
        if (errorOut) {
            *errorOut = "failed to parse job shell";
        }
        return false;
    }
    jobOut.clear_work_planes();

    for (int i = 0; i < jobLut.workplanepositions_size(); ++i) {
        const std::int64_t wpStart = jobLut.workplanepositions(i);
        ifs.seekg(static_cast<std::streamoff>(wpStart));
        std::int64_t wpLutPos = 0;
        if (!readLittleEndianI64(ifs, wpLutPos) || wpLutPos <= 0) {
            if (errorOut) {
                *errorOut = "invalid work plane LUT offset at plane " + std::to_string(i);
            }
            return false;
        }

        ifs.seekg(static_cast<std::streamoff>(wpLutPos));
        WorkPlaneLUT wpLut;
        if (!parseDelimited(wpLut, ifs)) {
            if (errorOut) {
                *errorOut = "failed to parse WorkPlaneLUT at plane " + std::to_string(i);
            }
            return false;
        }

        WorkPlane wp;
        ifs.seekg(static_cast<std::streamoff>(wpLut.workplaneshellposition()));
        if (!parseDelimited(wp, ifs)) {
            if (errorOut) {
                *errorOut = "failed to parse work plane shell at plane "
                    + std::to_string(i);
            }
            return false;
        }
        wp.clear_vector_blocks();

        for (int b = 0; b < wpLut.vectorblockspositions_size(); ++b) {
            ifs.seekg(static_cast<std::streamoff>(wpLut.vectorblockspositions(b)));
            VectorBlock* vb = wp.add_vector_blocks();
            if (!parseDelimited(*vb, ifs)) {
                if (errorOut) {
                    *errorOut = "failed to parse vector block "
                        + std::to_string(b) + " at plane " + std::to_string(i);
                }
                return false;
            }
        }
        wp.set_num_blocks(wp.vector_blocks_size());
        *jobOut.add_work_planes() = std::move(wp);
    }

    jobOut.set_num_work_planes(jobOut.work_planes_size());
    return true;
}

bool convertToOfficialOvfFile(const std::string& inputPath,
                              const std::string& outputPath,
                              std::string* errorOut)
{
    std::vector<char> data;
    if (!readFileBytes(inputPath, data, errorOut)) {
        return false;
    }

    if (looksLikeOfficialOvf(data)) {
        if (inputPath == outputPath) {
            return true;
        }
        std::ofstream ofs(pathFromUtf8(outputPath), std::ios::binary | std::ios::trunc);
        if (!ofs) {
            if (errorOut) {
                *errorOut = "failed to create: " + outputPath;
            }
            return false;
        }
        if (!data.empty()) {
            ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
        }
        if (!ofs) {
            if (errorOut) {
                *errorOut = "failed to copy official ovf";
            }
            return false;
        }
        return true;
    }

    Job job;
    if (!job.ParseFromArray(data.data(), static_cast<int>(data.size()))) {
        if (errorOut) {
            *errorOut =
                "input is neither official LVF! ovf nor a raw Job protobuf dump";
        }
        return false;
    }

    return writeOfficialOvfFile(job, outputPath, errorOut);
}

}  // namespace open_vector_format
