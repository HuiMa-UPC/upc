#ifndef UPC_RAW_THUMB_FORMAT_HPP
#define UPC_RAW_THUMB_FORMAT_HPP

#include <cstdint>
#include <cstring>

namespace upc {

enum class RawThumbColorFormat : uint32_t {
    RGB565 = 1,
};

constexpr char kRawThumbMagic[4] = {'R', 'T', 'H', 'M'};
constexpr uint16_t kRawThumbVersion = 1;

#pragma pack(push, 1)
struct RawThumbHeader {
    char     magic[4];
    uint16_t version;
    uint16_t reserved;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t color_format;
    uint32_t data_size;
};
#pragma pack(pop)

inline bool raw_thumb_header_valid(const RawThumbHeader& h)
{
    if(std::memcmp(h.magic, kRawThumbMagic, sizeof(kRawThumbMagic)) != 0) {
        return false;
    }
    if(h.version != kRawThumbVersion) {
        return false;
    }
    if(h.width == 0 || h.height == 0 || h.stride_bytes == 0 || h.data_size == 0) {
        return false;
    }
    if(h.color_format != static_cast<uint32_t>(RawThumbColorFormat::RGB565)) {
        return false;
    }
    if(h.data_size < h.stride_bytes * h.height) {
        return false;
    }
    return true;
}

} // namespace upc

#endif /* UPC_RAW_THUMB_FORMAT_HPP */
