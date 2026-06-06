#include "ipc_shm_region.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace upc_ipc {

IpcShmRegion::~IpcShmRegion()
{
    close();
}

IpcShmError IpcShmRegion::create(const char* name, size_t size)
{
    close();
    if (!name || !name[0] || size == 0) {
        return IpcShmError::InvalidArgument;
    }

    name_ = name;
    fd_   = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0660);
    if (fd_ < 0) {
        if (errno == EEXIST) {
            return IpcShmError::AlreadyExists;
        }
        return IpcShmError::IoError;
    }

    if (ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        close();
        shm_unlink(name);
        return IpcShmError::IoError;
    }

    base_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        base_ = nullptr;
        close();
        shm_unlink(name);
        return IpcShmError::MapFailed;
    }

    size_       = size;
    is_creator_ = true;
    return IpcShmError::Ok;
}

IpcShmError IpcShmRegion::open(const char* name, bool read_only)
{
    close();
    if (!name || !name[0]) {
        return IpcShmError::InvalidArgument;
    }

    name_ = name;
    const int flags = read_only ? O_RDONLY : O_RDWR;
    fd_             = shm_open(name, flags, 0);
    if (fd_ < 0) {
        return IpcShmError::IoError;
    }

    struct stat st {};
    if (fstat(fd_, &st) != 0 || st.st_size <= 0) {
        close();
        return IpcShmError::IoError;
    }

    size_ = static_cast<size_t>(st.st_size);
    const int prot = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
    base_          = mmap(nullptr, size_, prot, MAP_SHARED, fd_, 0);
    if (base_ == MAP_FAILED) {
        base_ = nullptr;
        close();
        return IpcShmError::MapFailed;
    }

    is_creator_ = false;
    return IpcShmError::Ok;
}

void IpcShmRegion::close()
{
    if (base_ && base_ != MAP_FAILED) {
        munmap(base_, size_);
    }
    base_ = nullptr;
    size_ = 0;

    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    is_creator_ = false;
    name_.clear();
}

IpcShmError IpcShmRegion::unlink_name() const
{
    if (name_.empty()) {
        return IpcShmError::InvalidArgument;
    }
    if (shm_unlink(name_.c_str()) != 0) {
        return IpcShmError::IoError;
    }
    return IpcShmError::Ok;
}

} // namespace upc_ipc
