#ifndef LOG_ROTATING_HPP
#define LOG_ROTATING_HPP

#include <cstddef>

/**
 * 三文件循环日志（每文件上限 5MB，顺序 1→2→3→1…）。
 * 进程 start / daemonize 时 init + 重定向 stdout/stderr；断电重启仅 append，不清空已有文件。
 */
void log_rotating_init(const char* log_base);

/** 是否已启用文件轮转（仅 daemon start 后为 true） */
bool log_rotating_active();

/** 写日志前调用：必要时切换/截断下一个 log 文件 */
void log_rotating_before_write(std::size_t next_len);

/** 写日志并 fflush 后调用：同步当前文件大小 */
void log_rotating_after_write();

#endif
