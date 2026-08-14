#include "log_rotating.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::size_t kMaxBytes = 20U * 1024U * 1024U;
constexpr int kMaxFiles = 3;
constexpr int kPathMax = 256;

std::mutex g_mutex;
char g_log_base[128] = {};
bool g_active = false;
int g_current_idx = 1;
std::size_t g_current_size = 0;

enum class OpenMode {
    Append,   /**< 启动 / 续写：O_APPEND，不丢已有内容 */
    Truncate, /**< 仅运行中轮转到下一份，或三份皆满时不得不腾出一份 */
};

void build_path(int idx, char* out, std::size_t out_sz)
{
    std::snprintf(out, out_sz, "%s%d.log", g_log_base, idx);
}

struct FileStat {
    bool exists = false;
    off_t size = 0;
    time_t mtime = 0;
};

void stat_log_file(int idx, FileStat* out)
{
    char path[kPathMax];
    build_path(idx, path, sizeof(path));
    struct stat st{};
    if(stat(path, &st) != 0) {
        out->exists = false;
        out->size = 0;
        out->mtime = 0;
        return;
    }
    out->exists = true;
    out->size = st.st_size;
    out->mtime = st.st_mtime;
}

/** O_APPEND 下 SEEK_CUR 常为 0，必须用 fstat 取真实大小 */
void sync_size_from_stderr()
{
    struct stat st{};
    if(fstat(STDERR_FILENO, &st) == 0 && st.st_size >= 0) {
        g_current_size = static_cast<std::size_t>(st.st_size);
    }
}

/** 确保 log_base 父目录存在（如 /data） */
bool mkdir_parent_of_base()
{
    if(!g_log_base[0]) {
        return false;
    }
    char dir[kPathMax];
    std::snprintf(dir, sizeof(dir), "%s", g_log_base);
    char* slash = std::strrchr(dir, '/');
    if(!slash || slash == dir) {
        return true; /* 相对路径或根下文件名，无需建目录 */
    }
    *slash = '\0';
    if(dir[0] == '\0') {
        return true;
    }

    /* 逐级 mkdir */
    for(char* p = dir + 1; *p; ++p) {
        if(*p != '/') {
            continue;
        }
        *p = '\0';
        if(mkdir(dir, 0755) != 0 && errno != EEXIST) {
            *p = '/';
            return false;
        }
        *p = '/';
    }
    if(mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

bool install_log_fd(int idx, OpenMode mode)
{
    char path[kPathMax];
    build_path(idx, path, sizeof(path));

    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    if(mode == OpenMode::Append) {
        flags |= O_APPEND;
    } else {
        flags |= O_TRUNC;
    }

    const int fd = open(path, flags, 0644);
    if(fd < 0) {
        return false;
    }
    if(dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        close(fd);
        return false;
    }
    close(fd);

    g_current_idx = idx;
    if(mode == OpenMode::Truncate) {
        g_current_size = 0;
    } else {
        sync_size_from_stderr();
    }
    return true;
}

/**
 * 启动选文件：
 * - 有未满文件：优先「有内容且 mtime 最新」的续写；否则选最小编号空槽 append
 * - 三份皆满：轮转到 newest 的下一份并 Truncate（与运行中轮转相同，不可避免腾出一份）
 */
void pick_and_open_startup()
{
    FileStat files[kMaxFiles];
    for(int i = 0; i < kMaxFiles; ++i) {
        stat_log_file(i + 1, &files[i]);
    }

    int best_with_content = -1;
    time_t best_content_mt = 0;
    int best_empty = -1;

    for(int i = 1; i <= kMaxFiles; ++i) {
        const FileStat& f = files[i - 1];
        const off_t sz = f.exists ? f.size : 0;
        if(sz >= static_cast<off_t>(kMaxBytes)) {
            continue;
        }
        if(sz > 0) {
            const time_t mt = f.mtime;
            if(best_with_content < 0 || mt > best_content_mt ||
               (mt == best_content_mt && i < best_with_content)) {
                best_with_content = i;
                best_content_mt = mt;
            }
        } else if(best_empty < 0 || i < best_empty) {
            best_empty = i;
        }
    }

    if(best_with_content > 0) {
        g_active = install_log_fd(best_with_content, OpenMode::Append);
        return;
    }
    if(best_empty > 0) {
        g_active = install_log_fd(best_empty, OpenMode::Append);
        return;
    }

    /* 三份皆满：腾出 newest 的下一份（仅此情况启动会 Truncate） */
    int newest = 1;
    time_t max_mt = 0;
    for(int i = 1; i <= kMaxFiles; ++i) {
        if(files[i - 1].exists && files[i - 1].mtime >= max_mt) {
            max_mt = files[i - 1].mtime;
            newest = i;
        }
    }
    const int next = (newest % kMaxFiles) + 1;
    g_active = install_log_fd(next, OpenMode::Truncate);
}

void rotate_to_next()
{
    const int next = (g_current_idx % kMaxFiles) + 1;
    (void)install_log_fd(next, OpenMode::Truncate);
}

} // namespace

void log_rotating_init(const char* log_base)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active = false;
    g_current_size = 0;
    g_current_idx = 1;
    g_log_base[0] = '\0';

    if(!log_base || !log_base[0]) {
        return;
    }
    std::strncpy(g_log_base, log_base, sizeof(g_log_base) - 1);
    g_log_base[sizeof(g_log_base) - 1] = '\0';

    (void)mkdir_parent_of_base();
    pick_and_open_startup();
}

bool log_rotating_active()
{
    return g_active;
}

void log_rotating_before_write(std::size_t next_len)
{
    if(!g_active) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);

    sync_size_from_stderr();
    if(g_current_size + next_len > kMaxBytes) {
        rotate_to_next();
    }
}

void log_rotating_after_write()
{
    if(!g_active) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    sync_size_from_stderr();
}
