#include "tcp_tx.hpp"
#include "tcp_frame.hpp"
#include <cstring>

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
    if(!out || out_size < 14) {
        return -1;
    }
    out[0] = TCP_SOF0;
    out[1] = TCP_SOF1;
    out[2] = 0x01;
    out[3] = 0x0B;
    std::memset(&out[4], 0, 10);
    return 14;
}

int tcp_build_reset_frame(uint8_t* out, size_t out_size)
{
    if(!out || out_size < 14) {
        return -1;
    }
    out[0] = TCP_SOF0;
    out[1] = TCP_SOF1;
    out[2] = 0x01;
    out[3] = 0x0A;
    std::memset(&out[4], 0, 10);
    return 14;
}
