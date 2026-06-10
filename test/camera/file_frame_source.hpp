#ifndef FILE_FRAME_SOURCE_HPP
#define FILE_FRAME_SOURCE_HPP

#include <cstddef>
#include <cstdint>
#include <string>

/** 从 NV12 裸流文件顺序读帧，支持循环播放 */
class FileFrameSource {
public:
    FileFrameSource() = default;
    ~FileFrameSource();

    FileFrameSource(const FileFrameSource&) = delete;
    FileFrameSource& operator=(const FileFrameSource&) = delete;

    bool open(const std::string& path, uint32_t width, uint32_t height);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return open_; }
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }
    [[nodiscard]] size_t frame_bytes() const noexcept { return frame_bytes_; }

    /** 读下一帧到 dst；文件结束且 loop=true 则回到开头 */
    bool read_frame(uint8_t* dst, size_t dst_size, bool loop);

private:
    bool rewind_to_start();

    int fd_ = -1;
    bool open_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    size_t frame_bytes_ = 0;
    std::string path_;
};

#endif /* FILE_FRAME_SOURCE_HPP */
