#ifndef UPC_IPC_SHM_REGION_HPP
#define UPC_IPC_SHM_REGION_HPP

#include "ipc_shm_types.hpp"
#include <cstddef>
#include <string>

namespace upc_ipc {

/** POSIX 共享内存映射（shm_open + mmap） */
class IpcShmRegion {
public:
    IpcShmRegion() = default;
    ~IpcShmRegion();

    IpcShmRegion(const IpcShmRegion&) = delete;
    IpcShmRegion& operator=(const IpcShmRegion&) = delete;

    IpcShmError create(const char* name, size_t size);
    IpcShmError open(const char* name, bool read_only);

    void close();
    IpcShmError unlink_name() const;

    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }
    [[nodiscard]] bool is_creator() const noexcept { return is_creator_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] const char* name() const noexcept { return name_.c_str(); }

    [[nodiscard]] void* data() noexcept { return base_; }
    [[nodiscard]] const void* data() const noexcept { return base_; }

private:
    std::string name_;
    void* base_ = nullptr;
    size_t size_ = 0;
    int fd_ = -1;
    bool is_creator_ = false;
};

} // namespace upc_ipc

#endif
