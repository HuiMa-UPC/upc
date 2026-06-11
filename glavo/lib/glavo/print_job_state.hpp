#ifndef PRINT_JOB_STATE_HPP
#define PRINT_JOB_STATE_HPP

#include <cstdint>
#include <mutex>

namespace upc::glavo {

struct PrintJobSnapshot {
    bool     is_sending      = false;
    bool     job_completed   = false;
    bool     job_error       = false;
    int      sent_packets    = 0;
    uint16_t cmd_cache_rate  = 0;
    uint16_t data_cache_rate = 0;
};

class PrintJobState {
public:
    static PrintJobState& instance();

    PrintJobSnapshot snapshot() const;

    void set_sending(bool v);
    void set_job_completed(bool v);
    void set_job_error(bool v);
    void set_sent_packets(int n);
    void set_mcu_rates(uint16_t cmd, uint16_t data);
    void reset_job_counters();

private:
    PrintJobState() = default;

    mutable std::mutex mtx_;
    PrintJobSnapshot snap_;
};

} // namespace upc::glavo

#endif
