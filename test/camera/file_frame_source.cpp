#include "file_frame_source.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

bool FileFrameSource::open(const std::string& path, uint32_t width, uint32_t height)
{
    close();
    if(path.empty() || width == 0 || height == 0) {
        return false;
    }

    fd_ = ::open(path.c_str(), O_RDONLY);
    if(fd_ < 0) {
        return false;
    }

    path_        = path;
    width_       = width;
    height_      = height;
    frame_bytes_ = static_cast<size_t>(width) * height * 3u / 2u;
    open_        = true;
    return true;
}

void FileFrameSource::close()
{
    if(fd_ >= 0) {
        ::close(fd_);
    }
    fd_          = -1;
    open_        = false;
    width_       = 0;
    height_      = 0;
    frame_bytes_ = 0;
    path_.clear();
}

FileFrameSource::~FileFrameSource()
{
    close();
}

bool FileFrameSource::rewind_to_start()
{
    if(fd_ < 0) {
        return false;
    }
    if(lseek(fd_, 0, SEEK_SET) < 0) {
        return false;
    }
    return true;
}

bool FileFrameSource::read_frame(uint8_t* dst, size_t dst_size, bool loop)
{
    if(!open_ || fd_ < 0 || !dst || dst_size < frame_bytes_) {
        return false;
    }

    size_t total = 0;
    while(total < frame_bytes_) {
        const ssize_t n = ::read(fd_, dst + total, frame_bytes_ - total);
        if(n > 0) {
            total += static_cast<size_t>(n);
            continue;
        }
        if(n == 0) {
            if(!loop) {
                return false;
            }
            if(!rewind_to_start()) {
                return false;
            }
            continue;
        }
        if(errno == EINTR) {
            continue;
        }
        return false;
    }

    return total == frame_bytes_;
}
