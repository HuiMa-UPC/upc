#include "upc_sys_monitor.hpp"
#include "rs_log.hpp"

#if UPC_DEBUG && UPC_MOD_SYS_MONITOR

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kReportInterval = std::chrono::minutes(1);

struct CpuSample {
    uint64_t total_jiffies = 0;
    uint64_t idle_jiffies  = 0;
    uint64_t proc_jiffies  = 0;
};

bool read_system_cpu(CpuSample* out) {
    if (!out) {
        return false;
    }
    FILE* fp = std::fopen("/proc/stat", "r");
    if (!fp) {
        return false;
    }
    char line[256];
    if (!std::fgets(line, sizeof(line), fp)) {
        std::fclose(fp);
        return false;
    }
    std::fclose(fp);

    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;
    if (std::sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
            &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) < 4) {
        return false;
    }

    out->total_jiffies = user + nice + system + idle + iowait + irq + softirq + steal;
    out->idle_jiffies  = idle + iowait;
    return true;
}

bool read_process_cpu(uint64_t* proc_jiffies) {
    if (!proc_jiffies) {
        return false;
    }
    FILE* fp = std::fopen("/proc/self/stat", "r");
    if (!fp) {
        return false;
    }
    char line[512];
    if (!std::fgets(line, sizeof(line), fp)) {
        std::fclose(fp);
        return false;
    }
    std::fclose(fp);

    const char* rp = std::strrchr(line, ')');
    if (!rp) {
        return false;
    }

    unsigned long long utime = 0;
    unsigned long long stime = 0;
    if (std::sscanf(rp + 2,
            " %*s %*d %*d %*d %*d %*d %*u %*u %*u %*u %llu %llu",
            &utime, &stime) != 2) {
        return false;
    }

    *proc_jiffies = utime + stime;
    return true;
}

bool read_cpu_sample(CpuSample* out) {
    if (!out) {
        return false;
    }
    if (!read_system_cpu(out)) {
        return false;
    }
    return read_process_cpu(&out->proc_jiffies);
}

bool read_meminfo(unsigned long* total_kb, unsigned long* avail_kb) {
    if (!total_kb || !avail_kb) {
        return false;
    }
    *total_kb = 0;
    *avail_kb = 0;

    FILE* fp = std::fopen("/proc/meminfo", "r");
    if (!fp) {
        return false;
    }

    char key[64];
    unsigned long value = 0;
    char unit[8];
    while (std::fscanf(fp, "%63s %lu %7s", key, &value, unit) == 3) {
        if (std::strcmp(key, "MemTotal:") == 0) {
            *total_kb = value;
        } else if (std::strcmp(key, "MemAvailable:") == 0) {
            *avail_kb = value;
        }
        if (*total_kb > 0 && *avail_kb > 0) {
            break;
        }
    }
    std::fclose(fp);
    return *total_kb > 0;
}

bool read_process_rss_kb(unsigned long* rss_kb) {
    if (!rss_kb) {
        return false;
    }
    *rss_kb = 0;

    FILE* fp = std::fopen("/proc/self/status", "r");
    if (!fp) {
        return false;
    }

    char line[128];
    while (std::fgets(line, sizeof(line), fp)) {
        unsigned long value = 0;
        if (std::sscanf(line, "VmRSS: %lu kB", &value) == 1) {
            *rss_kb = value;
            std::fclose(fp);
            return true;
        }
    }
    std::fclose(fp);
    return false;
}

bool read_cpu_temp_c(float* temp_c) {
    if (!temp_c) {
        return false;
    }
    *temp_c = 0.f;

    static const char* kPaths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/thermal/thermal_zone1/temp",
    };

    for (const char* path : kPaths) {
        FILE* fp = std::fopen(path, "r");
        if (!fp) {
            continue;
        }
        int millideg = 0;
        if (std::fscanf(fp, "%d", &millideg) == 1 && millideg > 0) {
            *temp_c = static_cast<float>(millideg) / 1000.f;
            std::fclose(fp);
            return true;
        }
        std::fclose(fp);
    }
    return false;
}

double cpu_usage_pct(uint64_t delta_busy, uint64_t delta_total) {
    if (delta_total == 0) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(delta_busy) / static_cast<double>(delta_total);
}

void log_sample(const CpuSample& prev, const CpuSample& cur) {
    const uint64_t total_delta = cur.total_jiffies - prev.total_jiffies;
    const uint64_t idle_delta  = cur.idle_jiffies - prev.idle_jiffies;
    const uint64_t proc_delta  = cur.proc_jiffies - prev.proc_jiffies;

    const double cpu_sys_pct  = cpu_usage_pct(total_delta - idle_delta, total_delta);
    const double cpu_proc_pct = cpu_usage_pct(proc_delta, total_delta);

    unsigned long mem_total_kb = 0;
    unsigned long mem_avail_kb = 0;
    read_meminfo(&mem_total_kb, &mem_avail_kb);

    unsigned long rss_kb = 0;
    read_process_rss_kb(&rss_kb);

    float temp_c = 0.f;
    const bool have_temp = read_cpu_temp_c(&temp_c);

    if (have_temp) {
        UPC_LOG_SYS_MONITOR(Info,
            "cpu_sys=%.1f%% cpu_proc=%.1f%% temp=%.1fC mem_avail=%luMB/%luMB rss=%luMB",
            cpu_sys_pct,
            cpu_proc_pct,
            temp_c,
            mem_avail_kb / 1024UL,
            mem_total_kb / 1024UL,
            rss_kb / 1024UL);
    } else {
        UPC_LOG_SYS_MONITOR(Warn,
            "cpu_sys=%.1f%% cpu_proc=%.1f%% temp=N/A mem_avail=%luMB/%luMB rss=%luMB",
            cpu_sys_pct,
            cpu_proc_pct,
            mem_avail_kb / 1024UL,
            mem_total_kb / 1024UL,
            rss_kb / 1024UL);
    }
}

} // namespace

void upc_sys_monitor_tick() {
    static Clock::time_point last_report{};
    static bool have_base = false;
    static CpuSample base{};

    /*
     * 先判间隔再读 /proc。本函数在 UI 主循环里每圈都被调用（上百次每秒），
     * 而 read_cpu_sample 要开 /proc/stat + /proc/self/stat 两个文件，采样值
     * 只有到了上报点才用得上——把读放在间隔判断之后，绝大多数调用就只剩
     * 一次时钟比较。CPU 占用是「两次采样之间的均值」，基准点仍是上一次上报
     * 时刻，语义不变。
     */
    const auto now = Clock::now();
    if (have_base && now - last_report < kReportInterval) {
        return;
    }

    CpuSample cur{};
    if (!read_cpu_sample(&cur)) {
        return;
    }

    if (!have_base) {
        base = cur;
        have_base = true;
        last_report = now;
        return;
    }

    log_sample(base, cur);
    base = cur;
    last_report = now;
}

#else /* !(UPC_DEBUG && UPC_MOD_SYS_MONITOR) */

void upc_sys_monitor_tick() {}

#endif /* UPC_DEBUG && UPC_MOD_SYS_MONITOR */
