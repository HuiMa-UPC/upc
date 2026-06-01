#ifndef PRINT_JOB_STATE_HPP
#define PRINT_JOB_STATE_HPP

#include "ring_buffer.hpp"
#include <cstdint>
#include <mutex>

namespace services {

struct PrintJobSnapshot {
    bool     tcp_connected   = false;
    bool     serial_open     = false;
    bool     is_sending      = false;
    bool     job_completed   = false;
    bool     job_error       = false;
    int      total_packets   = 0;
    int      sent_packets    = 0;
    uint16_t cmd_cache_rate  = 0;
    uint16_t data_cache_rate = 0;
};

class PrintJobState {
public:
    static PrintJobState& instance();

    PrintJobSnapshot snapshot() const;

    void set_tcp_connected(bool v);
    void set_serial_open(bool v);
    void set_sending(bool v);
    void set_job_completed(bool v);
    void set_job_error(bool v);
    void set_total_packets(int n);
    void set_sent_packets(int n);
    void set_mcu_rates(uint16_t cmd, uint16_t data);
    void reset_job_counters();

private:
    PrintJobState() = default;

    mutable std::mutex mtx_;
    PrintJobSnapshot snap_;
};

/** RB_PKT_LAYER_MARK：data[0..1] 为层厚 uint16 LE (μm) */
inline void rb_layer_mark_set_height_um(rb_packet_t& pkt, uint16_t um)
{
    pkt.data[0] = (uint8_t)(um & 0xFF);
    pkt.data[1] = (uint8_t)(um >> 8);
}

inline uint16_t rb_layer_mark_get_height_um(const rb_packet_t& pkt)
{
    return (uint16_t)pkt.data[0] | ((uint16_t)pkt.data[1] << 8);
}

/** G-code Z 模态值 (μm) → uint16，无效时返回 0 */
inline uint16_t gcode_z_to_layer_um(float z_um)
{
    if(z_um <= 0.f) {
        return 0;
    }
    const float rounded = z_um + 0.5f;
    if(rounded > 65535.f) {
        return 65535;
    }
    return (uint16_t)rounded;
}

} // namespace services

#endif
