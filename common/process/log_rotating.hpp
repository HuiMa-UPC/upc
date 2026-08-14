#ifndef LOG_ROTATING_HPP
#define LOG_ROTATING_HPP

#include <cstddef>

/**
 * 三文件循环日志（每文件上限 20MB，顺序 1→2→3→1…）。
 *
 * 启动 / 重启：
 * - 未满的文件一律 O_APPEND 续写，不清空已有内容
 * - 优先续写「有内容且最近写过」的那份；避免被空文件的新 mtime 抢走
 * - 仅当 1/2/3 皆已满时，才 Truncate 下一份腾出空间（与运行中轮转相同）
 *
 * 大小跟踪用 fstat（O_APPEND 下 SEEK_CUR 不可靠）。
 * init 时会 mkdir 日志路径父目录（如 /data）。
 */
void log_rotating_init(const char* log_base);

/** 是否已启用文件轮转（仅 daemon start 后为 true） */
bool log_rotating_active();

/** 写日志前调用：当前文件将超过上限则轮转到下一份（Truncate 下一份） */
void log_rotating_before_write(std::size_t next_len);

/** 写日志并 fflush 后调用：同步当前文件大小 */
void log_rotating_after_write();

#endif
