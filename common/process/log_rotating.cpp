#include "log_rotating.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr std::size_t kMaxBytes = 5U * 1024U * 1024U;
constexpr int kMaxFiles = 3;
constexpr int kPathMax = 256;

std::mutex g_mutex;
char g_log_base[128] = {};
bool g_active = false;
int g_current_idx = 1;
std::size_t g_current_size = 0;
bool g_pending_truncate = false;

enum class OpenMode {
    Append,
    Truncate,
    DeferredTruncate,
};

void build_path(int idx, char* out, std::size_t out_sz) {
    std::snprintf(out, out_sz, "%s%d.log", g_log_base, idx);
}

struct FileStat {
    bool exists = false;
    off_t size = 0;
    time_t mtime = 0;
};

void stat_log_file(int idx, FileStat* out) {
    char path[kPathMax];
    build_path(idx, path, sizeof(path));
    struct stat st{};
    if (stat(path, &st) != 0) {
        out->exists = false;
        out->size = 0;
        out->mtime = 0;
        return;
    }
    out->exists = true;
    out->size = st.st_size;
    out->mtime = st.st_mtime;
}

void sync_size_from_stderr() {
    const off_t pos = lseek(STDERR_FILENO, 0, SEEK_CUR);
    if (pos >= 0) {
        g_current_size = static_cast<std::size_t>(pos);
    }
}

bool install_log_fd(int idx, OpenMode mode) {
    char path[kPathMax];
    build_path(idx, path, sizeof(path));

    int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
    switch (mode) {
        case OpenMode::Append:
            flags |= O_APPEND;
            break;
        case OpenMode::Truncate:
            flags |= O_TRUNC;
            break;
        case OpenMode::DeferredTruncate:
            break;
    }

    const int fd = open(path, flags, 0644);
    if (fd < 0) {
        return false;
    }
    if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        close(fd);
        return false;
    }
    close(fd);

    g_current_idx = idx;
    if (mode == OpenMode::Truncate) {
        g_current_size = 0;
        g_pending_truncate = false;
    } else if (mode == OpenMode::DeferredTruncate) {
        sync_size_from_stderr();
    } else {
        sync_size_from_stderr();
    }
    return true;
}

int pick_startup_index(bool* pending_truncate) {
    *pending_truncate = false;

    FileStat files[kMaxFiles];
    for (int i = 0; i < kMaxFiles; ++i) {
        stat_log_file(i + 1, &files[i]);
    }

    int best_idx = -1;
    time_t best_mtime = 0;
    for (int i = 1; i <= kMaxFiles; ++i) {
        const FileStat& f = files[i - 1];
        const off_t sz = f.exists ? f.size : 0;
        if (sz >= static_cast<off_t>(kMaxBytes)) {
            continue;
        }
        const time_t mt = f.exists ? f.mtime : 0;
        if (best_idx < 0 || mt > best_mtime || (mt == best_mtime && i < best_idx)) {
            best_idx = i;
            best_mtime = mt;
        }
    }
    if (best_idx > 0) {
        return best_idx;
    }

    int newest = 1;
    time_t max_mt = 0;
    for (int i = 1; i <= kMaxFiles; ++i) {
        if (files[i - 1].exists && files[i - 1].mtime >= max_mt) {
            max_mt = files[i - 1].mtime;
            newest = i;
        }
    }
    *pending_truncate = true;
    return (newest % kMaxFiles) + 1;
}

void apply_pending_truncate() {
    if (ftruncate(STDERR_FILENO, 0) == 0) {
        lseek(STDERR_FILENO, 0, SEEK_SET);
    }
    g_current_size = 0;
    g_pending_truncate = false;
}

void rotate_to_next() {
    const int next = (g_current_idx % kMaxFiles) + 1;
    install_log_fd(next, OpenMode::Truncate);
}

} // namespace

void log_rotating_init(const char* log_base) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_active = false;
    g_pending_truncate = false;
    g_current_size = 0;
    g_current_idx = 1;
    g_log_base[0] = '\0';

    if (!log_base || !log_base[0]) {
        return;
    }
    std::strncpy(g_log_base, log_base, sizeof(g_log_base) - 1);
    g_log_base[sizeof(g_log_base) - 1] = '\0';

    bool pending = false;
    const int idx = pick_startup_index(&pending);
    g_pending_truncate = pending;
    const OpenMode mode = pending ? OpenMode::DeferredTruncate : OpenMode::Append;
    g_active = install_log_fd(idx, mode);
}

bool log_rotating_active() {
    return g_active;
}

void log_rotating_before_write(std::size_t next_len) {
    if (!g_active) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_pending_truncate) {
        apply_pending_truncate();
        return;
    }

    sync_size_from_stderr();
    if (g_current_size + next_len > kMaxBytes) {
        rotate_to_next();
    }
}

void log_rotating_after_write() {
    if (!g_active) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    sync_size_from_stderr();
}
