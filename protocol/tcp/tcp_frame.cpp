#include "tcp_frame.hpp"

size_t tcp_guess_rx_frame_len(const uint8_t* buf, size_t avail)
{
    if(!buf || avail < 3) {
        return 0;
    }
    if(buf[0] != TCP_SOF0 || buf[1] != TCP_SOF1) {
        return 1;
    }

    const uint8_t data_len = buf[2];
    const size_t total = 3u + (size_t)data_len;
    if(total < 4) {
        return 1;
    }
    return avail < total ? 0 : total;
}
