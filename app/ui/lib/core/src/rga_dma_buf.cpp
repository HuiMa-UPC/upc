#include "rga_dma_buf.hpp"
#include "dma_heap_buffer.hpp"
#include "rs_log.hpp"

#include <cerrno>
#include <cstring>

namespace ui {

RgaDmaBuf::~RgaDmaBuf()
{
    free();
}

RgaDmaBuf::RgaDmaBuf(RgaDmaBuf&& other) noexcept
    : fd_(other.fd_)
    , va_(other.va_)
    , size_(other.size_)
    , heap_path_(other.heap_path_)
{
    other.fd_        = -1;
    other.va_        = nullptr;
    other.size_      = 0;
    other.heap_path_ = nullptr;
}

RgaDmaBuf& RgaDmaBuf::operator=(RgaDmaBuf&& other) noexcept
{
    if(this != &other) {
        free();
        fd_        = other.fd_;
        va_        = other.va_;
        size_      = other.size_;
        heap_path_ = other.heap_path_;
        other.fd_        = -1;
        other.va_        = nullptr;
        other.size_      = 0;
        other.heap_path_ = nullptr;
    }
    return *this;
}

bool RgaDmaBuf::alloc(size_t size)
{
    free();
    if(size == 0) {
        return false;
    }

    upc_ipc::DmaHeapBuffer hb{};
    if(!upc_ipc::dma_heap_alloc(size, hb)) {
        UPC_LOG_CAMERA_PREVIEW(Error,
            "RGA dma heap alloc failed size=%zu (%s)",
            size, std::strerror(errno));
        return false;
    }

    fd_        = hb.fd;
    va_        = hb.va;
    size_      = hb.size;
    heap_path_ = hb.heap_path;

    static bool logged{false};
    if(!logged) {
        logged = true;
        UPC_LOG_CAMERA_PREVIEW(Info, "RGA dma heap using %s",
                               heap_path_ ? heap_path_ : "?");
    }
    return true;
}

void RgaDmaBuf::free()
{
    upc_ipc::DmaHeapBuffer hb{fd_, va_, size_, heap_path_};
    upc_ipc::dma_heap_free(hb);

    fd_        = -1;
    va_        = nullptr;
    size_      = 0;
    heap_path_ = nullptr;
}

} // namespace ui
