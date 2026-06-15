#include "tcp_rx.hpp"
#include "tcp_frame.hpp"

int tcp_parse_status_response(const uint8_t* data, size_t len, tcp_response_t* resp)
{
    if(!data || len < 19 || !resp) {
        return -1;
    }
    if(data[0] != TCP_SOF0 || data[1] != TCP_SOF1 ||
       data[2] != 0x10 || data[3] != 0x01) {
        return -1;
    }

    resp->cmd_rate  = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    resp->data_rate = (uint16_t)data[6] | ((uint16_t)data[7] << 8);
    return 0;
}
