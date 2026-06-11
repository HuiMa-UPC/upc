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
    /** 析构函数：自动关闭映射并释放资源 */
    ~IpcShmRegion();

    IpcShmRegion(const IpcShmRegion&) = delete;
    IpcShmRegion& operator=(const IpcShmRegion&) = delete;

    /**
     * 创建新的 POSIX 共享内存对象（shm_open + ftruncate + mmap）。
     * 若同名对象已存在则返回 AlreadyExists。
     * @param name 共享内存对象名称（需以 '/' 开头）
     * @param size 共享内存区域字节大小
     * @return 成功返回 Ok，否则返回对应错误码
     */
    IpcShmError create(const char* name, size_t size);
    /**
     * 打开已存在的 POSIX 共享内存对象并执行 mmap 映射。
     * @param name       共享内存对象名称
     * @param read_only  是否以只读方式映射
     * @return 成功返回 Ok，否则返回对应错误码
     */
    IpcShmError open(const char* name, bool read_only);

    /** 解除内存映射、关闭文件描述符并清理内部状态 */
    void close();
    /**
     * 从系统中删除该共享内存对象（shm_unlink）。
     * 通常仅由创建者调用。
     * @return 成功返回 Ok，否则返回对应错误码
     */
    IpcShmError unlink_name() const;

    /** 判断当前共享内存映射是否有效 */
    [[nodiscard]] bool valid() const noexcept { return base_ != nullptr; }
    /** 判断当前实例是否为该共享内存的创建者 */
    [[nodiscard]] bool is_creator() const noexcept { return is_creator_; }
    /** 获取映射的共享内存字节大小 */
    [[nodiscard]] size_t size() const noexcept { return size_; }
    /** 获取共享内存对象名称 */
    [[nodiscard]] const char* name() const noexcept { return name_.c_str(); }

    /** 获取映射区域的可写数据指针 */
    [[nodiscard]] void* data() noexcept { return base_; }
    /** 获取映射区域的只读数据指针 */
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
