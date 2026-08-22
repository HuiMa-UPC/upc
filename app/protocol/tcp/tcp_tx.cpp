#include "tcp_tx.hpp"
#include "tcp_frame.hpp"
#include <cstring>

namespace {

/** 单 FRAME 子包（11B）：BE EF 01 + CMD + payload[10] */
int tcp_build_frame11(uint8_t cmd, const uint8_t payload[10], uint8_t* out, size_t out_size)
{
    if(!out || out_size < 14) {
        return -1;
    }
    out[0] = TCP_SOF0;
    out[1] = TCP_SOF1;
    out[2] = 0x01;
    out[3] = cmd;
    if(payload) {
        std::memcpy(&out[4], payload, 10);
    } else {
        std::memset(&out[4], 0, 10);
    }
    return 14;
}

} // namespace

int tcp_build_motion_frame(const uint8_t* subpkts11, int n,
                           uint8_t* out, size_t out_size)
{
    if(!subpkts11 || n <= 0 || n > 5957 || !out ||
       out_size < (size_t)(3 + 11 * n)) {
        return -1;
    }

    out[0] = TCP_SOF0;
    out[1] = TCP_SOF1;
    out[2] = (uint8_t)n;
    std::memcpy(&out[3], subpkts11, (size_t)n * 11u);
    return 3 + n * 11;
}

int tcp_build_heartbeat_frame(uint8_t* out, size_t out_size)
{
    return tcp_build_frame11(0x0B, nullptr, out, out_size);
}

int tcp_build_emergency_frame_ex(uint8_t stop, uint8_t pause, uint8_t red_laser,
                                 uint8_t* out, size_t out_size)
{
    uint8_t payload[10]{};
    payload[0] = stop;
    payload[1] = pause;
    payload[2] = red_laser;
    return tcp_build_frame11(0x0A, payload, out, out_size);
}

int tcp_build_emergency_frame(uint8_t stop, uint8_t pause,
                              uint8_t* out, size_t out_size)
{
    return tcp_build_emergency_frame_ex(stop, pause, 0u, out, out_size);
}

int tcp_build_reset_frame(uint8_t* out, size_t out_size)
{
    return tcp_build_emergency_frame(1u, 0u, out, out_size);
}

int tcp_build_ota_ctrl_frame(uint8_t mode, uint8_t* out, size_t out_size)
{
    uint8_t payload[10]{};
    payload[0] = mode;
    return tcp_build_frame11(0x03, payload, out, out_size);
}

int tcp_build_ota_data_frame(uint16_t seq, const uint8_t* data, uint16_t len,
                             uint8_t* out, size_t out_size)
{
    if(!out || !data || len == 0) {
        return -1;
    }
    const size_t sub_len = 5u + static_cast<size_t>(len);
    const size_t total   = 3u + sub_len;
    if(out_size < total) {
        return -1;
    }

    out[0] = TCP_SOF0;
    out[1] = TCP_SOF1;
    out[2] = 0x01;
    out[3] = 0x13;
    out[4] = static_cast<uint8_t>(len & 0xFFu);
    out[5] = static_cast<uint8_t>((len >> 8) & 0xFFu);
    out[6] = static_cast<uint8_t>(seq & 0xFFu);
    out[7] = static_cast<uint8_t>((seq >> 8) & 0xFFu);
    std::memcpy(&out[8], data, len);
    return static_cast<int>(total);
}
