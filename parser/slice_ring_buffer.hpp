#ifndef SLICE_RING_BUFFER_HPP
#define SLICE_RING_BUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace upc::parser {

/** 解析后运动指令队列容量（约 12B/包，65536≈768KB） */
#define RB_MAX_PACKETS    65536
#define RB_PKT_DATA_LEN   11

typedef enum {
    RB_PKT_MOTION = 0,
    RB_PKT_LAYER_MARK = 1,
} rb_pkt_type_t;

typedef struct {
    uint8_t type;                     /* RB_PKT_MOTION or RB_PKT_LAYER_MARK */
    uint8_t data[RB_PKT_DATA_LEN];    /* 11 bytes motion subpack when type==MOTION */
} rb_packet_t;

class RingBuffer {
public:
    RingBuffer();

    /** Wait for space; return false if stop_flag becomes true while full */
    bool put_wait(const rb_packet_t& pkt, const std::atomic<bool>& stop_flag);

    /* Consumer: blocks until timeout_ms if empty */
    bool get(rb_packet_t& pkt, int timeout_ms);

    /* Non-blocking peek at next packet type */
    bool peek_type(rb_pkt_type_t& type) const;

    bool empty() const;
    size_t count() const;

    void clear();

private:
    mutable std::mutex mtx;
    std::condition_variable cv_not_full;
    std::condition_variable cv_not_empty;

    rb_packet_t buf[RB_MAX_PACKETS];
    size_t head = 0;
    size_t tail = 0;
    size_t cnt  = 0;
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

} // namespace upc::parser

#endif /* SLICE_RING_BUFFER_HPP */
