#include "print_job_state.hpp"

namespace services {

PrintJobState& PrintJobState::instance()
{
    static PrintJobState inst;
    return inst;
}

PrintJobSnapshot PrintJobState::snapshot() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return snap_;
}

void PrintJobState::set_tcp_connected(bool v)
{
    std::lock_guard<std::mutex> lock(mtx_);
    snap_.tcp_connected = v;
}

void PrintJobState::set_serial_open(bool v)
{
    std::lock_guard<std::mutex> lock(mtx_);
    snap_.serial_open = v;
}

void PrintJobState::set_sending(bool v)
{
    std::lock_guard<std::mutex> lock(mtx_);
    snap_.is_sending = v;
}

void PrintJobState::set_job_completed(bool v)
{
    std::lock_guard<std::mutex> lock(mtx_);
    snap_.job_completed = v;
}

void PrintJobState::set_job_error(bool v)
{
    std::lock_guard<std::mutex> lock(mtx_);
    snap_.job_error = v;
}

void PrintJobState::set_total_packets(int n)
{
    std::lock_guard<std::mutex> lock(mtx_);
    snap_.total_packets = n;
}

void PrintJobState::set_sent_packets(int n)
{
    std::lock_guard<std::mutex> lock(mtx_);
    snap_.sent_packets = n;
}

void PrintJobState::set_mcu_rates(uint16_t cmd, uint16_t data)
{
    std::lock_guard<std::mutex> lock(mtx_);
    snap_.cmd_cache_rate  = cmd;
    snap_.data_cache_rate = data;
}

void PrintJobState::reset_job_counters()
{
    std::lock_guard<std::mutex> lock(mtx_);
    snap_.total_packets   = 0;
    snap_.sent_packets    = 0;
    snap_.job_completed   = false;
    snap_.job_error       = false;
    snap_.cmd_cache_rate  = 0;
    snap_.data_cache_rate = 0;
}

} // namespace services
