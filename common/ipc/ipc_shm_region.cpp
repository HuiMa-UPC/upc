#include "ipc_shm_region.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace upc_ipc {

/** 析构：确保映射被安全关闭 */
IpcShmRegion::~IpcShmRegion()
{
    close();
}

/**
 * 创建共享内存对象：
 * 1. 以 O_CREAT | O_EXCL | O_RDWR 创建，防止覆盖已有对象；
 * 2. ftruncate 设置大小；
 * 3. mmap 映射到进程地址空间。
 * 若任何步骤失败则自动回滚（unlink / close）。
 */
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

/**
 * 打开已有共享内存对象：
 * 1. shm_open 获取文件描述符；
 * 2. fstat 获取实际大小；
 * 3. mmap 按指定权限映射。
 */
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

/**
 * 关闭映射：先 munmap 解除内存映射，再关闭 fd，最后重置所有内部状态。
 * 注意：不会执行 shm_unlink，如需删除对象请调用 unlink_name()。
 */
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

/**
 * 调用 shm_unlink 删除系统中的共享内存对象。
 * 仅在 name_ 非空时执行，失败返回 IoError。
 */
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
