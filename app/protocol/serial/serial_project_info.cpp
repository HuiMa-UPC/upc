#include "serial_project_info.hpp"
#include "serial_frame.hpp"

int serial_build_project_info_request(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SERIAL_PINFO_PORT_GENERAL, SERIAL_PINFO_CMD,
                                 SERIAL_PINFO_TLV_GET, 0, nullptr, out, out_size);
}

int serial_parse_project_info_report(const uint8_t* data, size_t len,
                                     serial_project_info_t* out)
{
    /* 整帧 = 8 + payload_len(0x09) = 17：Value 7 字节 + 1 字节校验和 */
    if(!data || !out || len < 17) {
        return -1;
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return -2;
    }
    if(data[3] != SERIAL_PINFO_PORT_GENERAL || data[4] != SERIAL_PINFO_CMD) {
        return -2;
    }

    const uint8_t payload_len = data[6];
    if(payload_len != 0x09 || data[7] != SERIAL_PINFO_TLV_REPORT ||
       data[8] != SERIAL_PINFO_REPORT_TLV_LEN) {
        return -3;
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return -4;
    }

    const uint8_t* v = &data[9];
    out->major        = v[0];
    out->minor        = v[1];
    out->patch        = v[2];
    out->laser_type   = v[3];
    out->z_motor_type = v[4];
    out->run_slot     = v[5];
    out->ota_state    = v[6];
    return 0;
}
