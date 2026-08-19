#include "serial_alarm.hpp"
#include "serial_frame.hpp"

namespace {

inline uint16_t rd_le16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t rd_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

int serial_parse_alarm_single_report(const uint8_t* data, size_t len,
                                     serial_alarm_report_t* out)
{
    /* 整帧 = 8 + payload_len(0x0B) = 19：TLV 头 2B + Value 9B + 校验和 1B */
    if(!data || !out || len < 19) {
        return -1;
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return -2;
    }
    if(data[3] != SERIAL_ALARM_PORT || data[4] != SERIAL_ALARM_CMD_SINGLE_REPORT) {
        return -2;
    }

    const uint8_t payload_len = data[6];
    if(payload_len != 0x0B || data[7] != SERIAL_ALARM_TLV_SINGLE_REPORT ||
       data[8] != SERIAL_ALARM_SINGLE_TLV_LEN) {
        return -3;
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return -4;
    }

    const uint8_t* v = &data[9];
    out->alarm_code = rd_le16(v);
    out->severity   = v[2];
    out->state_mask = v[3];
    out->clear_mode = v[4];
    out->detail     = rd_le32(v + 5);
    return 0;
}

int serial_parse_alarm_snapshot(const uint8_t* data, size_t len,
                                serial_alarm_snapshot_t* out)
{
    /* 整帧 = 8 + payload_len(0x0D) = 21：TLV 头 2B + Value 11B + 校验和 1B */
    if(!data || !out || len < 21) {
        return -1;
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return -2;
    }
    if(data[3] != SERIAL_ALARM_PORT || data[4] != SERIAL_ALARM_CMD_EVT_REPORT) {
        return -2;
    }

    const uint8_t payload_len = data[6];
    if(payload_len != 0x0D || data[7] != SERIAL_ALARM_TLV_EVT_SNAPSHOT ||
       data[8] != SERIAL_ALARM_SNAPSHOT_TLV_LEN) {
        return -3;
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return -4;
    }

    const uint8_t* v = &data[9];
    const uint8_t count = v[0];
    const uint8_t slot  = v[1];
    /* 越界的 slot/count 说明对端版本对不上，整帧丢弃比按错位置解位图安全 */
    if(count > SERIAL_ALARM_SNAPSHOT_SLOTS || slot >= SERIAL_ALARM_SNAPSHOT_SLOTS) {
        return -5;
    }

    out->nonzero_slot_count = count;
    out->slot_id            = slot;
    out->highest_severity   = v[2];
    out->active_mask        = rd_le32(v + 3);
    out->latched_mask       = rd_le32(v + 7);
    return 0;
}

int serial_parse_alarm_reset_result(const uint8_t* data, size_t len,
                                    serial_alarm_reset_result_t* out)
{
    /* 整帧 = 8 + payload_len(0x05) = 13：TLV 头 2B + Value 3B + 校验和 1B */
    if(!data || !out || len < 13) {
        return -1;
    }
    if(data[0] != SERIAL_SOF0 || data[1] != SERIAL_SOF1 ||
       data[2] != SERIAL_FT_MCU_TO_SOC) {
        return -2;
    }
    /* TLV 0x04 与快照共号，Cmd 是唯一区分依据 */
    if(data[3] != SERIAL_ALARM_PORT || data[4] != SERIAL_ALARM_CMD_EVT_RESPONSE) {
        return -2;
    }

    const uint8_t payload_len = data[6];
    if(payload_len != 0x05 || data[7] != SERIAL_ALARM_TLV_EVT_RESET_RES ||
       data[8] != SERIAL_ALARM_RESET_RES_TLV_LEN) {
        return -3;
    }
    if(serial_frame_checksum_check(data, len, nullptr, nullptr) != 0) {
        return -4;
    }

    const uint8_t* v = &data[9];
    out->alarm_code = rd_le16(v);
    out->result     = v[2];
    return 0;
}

int serial_build_alarm_reset(uint16_t alarm_code, uint8_t* out, size_t out_size)
{
    const uint8_t value[2] = {
        static_cast<uint8_t>(alarm_code & 0xFF),
        static_cast<uint8_t>((alarm_code >> 8) & 0xFF),
    };
    return serial_build_aa_frame(SERIAL_ALARM_PORT, SERIAL_ALARM_CMD_EVT_RESPONSE,
                                 SERIAL_ALARM_TLV_EVT_RESET, sizeof(value), value,
                                 out, out_size);
}

int serial_build_alarm_snapshot_request(uint8_t* out, size_t out_size)
{
    return serial_build_aa_frame(SERIAL_ALARM_PORT, SERIAL_ALARM_CMD_EVT_REPORT,
                                 SERIAL_ALARM_TLV_EVT_REQSPT, 0, nullptr,
                                 out, out_size);
}
