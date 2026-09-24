#ifndef UPC_CABIN_DETECTOR_HPP
#define UPC_CABIN_DETECTOR_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace upc::visual {
/** Reject recent retransmissions, but do not retain a 32-bit millisecond ordering
 * fence beyond its comparison horizon after the session is fully stopped. */
inline bool cabin_session_start_allowed(uint32_t current, bool active,
                                       uint64_t stopped_ms, uint32_t candidate,
                                       uint64_t now_ms) {
    if(candidate == 0) return false;
    if(current == 0) return true;
    if(!active && now_ms >= stopped_ms && now_ms - stopped_ms >= 60000) return true;
    const uint32_t delta = candidate - current;
    return delta != 0 && delta < 0x80000000u;
}
struct CabinDetection {
    uint8_t result = 0;
    double score = 0.0;
    std::string reason = "unknown";
};

/** Single-frame compact-NV12 detectors. No motion, IPC, Z, or UI ownership. */
class CabinDetector {
public:
    CabinDetector();
    ~CabinDetector();
    CabinDetector(const CabinDetector&) = delete;
    CabinDetector& operator=(const CabinDetector&) = delete;
    // Both detectors require an explicitly loaded calibration. A failed reload
    // invalidates the respective detector instead of retaining stale settings.
    bool load_plate_config(const std::string& path, std::string& error);
    bool load_door_config(const std::string& path, std::string& error);
    CabinDetection door(const uint8_t* nv12, size_t bytes, uint32_t width,
                        uint32_t height, uint8_t led_mask) const;
    CabinDetection plate(const uint8_t* nv12, size_t bytes, uint32_t width,
                         uint32_t height, uint8_t led_mask) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool door_calibrated_ = false;
    double door_max_mean_ = 24, door_max_p95_ = 32, door_dark_threshold_ = 40,
           door_min_dark_fraction_ = .98, door_clear_mean_ = 40, door_clear_p95_ = 60;
};

/** Per-request temporal evidence. Duplicate/old frames never advance it.
 * Unknown breaks the streak. WaitOpen ignores clear and waits for full black. */
class CabinEvidence {
public:
    CabinEvidence(bool door, bool wait_open, uint32_t initial_seq,
                  uint64_t start_ms, uint32_t warmup_ms = 200)
        : door_(door), wait_open_(wait_open), last_seq_(initial_seq),
          start_ms_(start_ms), warmup_ms_(warmup_ms) {}
    uint8_t observe(uint8_t result, uint32_t seq, uint64_t now_ms) {
        const uint32_t delta = seq - last_seq_;
        if(seq == 0 || (last_seq_ && (delta == 0 || delta >= 0x80000000u))) return 0;
        last_seq_ = seq;
        if(now_ms < start_ms_ || now_ms - start_ms_ < warmup_ms_) {
            count_ = 0; last_result_ = 0; return 0;
        }
        if(result == 0 || result > 2 || (door_ && wait_open_ && result != 1)) {
            count_ = 0; last_result_ = 0; return 0;
        }
        if(result != last_result_ || now_ms < last_ms_ || now_ms - last_ms_ > 500) {
            count_ = 0; streak_ms_ = now_ms;
        }
        last_result_ = result; last_ms_ = now_ms; ++count_;
        const uint32_t frames = door_ ? 5 : (result == 1 ? 3 : 12);
        const uint32_t duration = door_ ? 150 : (result == 1 ? 100 : 1000);
        return count_ >= frames && now_ms - streak_ms_ >= duration ? result : 0;
    }
private:
    bool door_, wait_open_;
    uint32_t last_seq_;
    uint64_t start_ms_, streak_ms_ = 0, last_ms_ = 0;
    uint32_t warmup_ms_, count_ = 0;
    uint8_t last_result_ = 0;
};
} // namespace upc::visual
#endif
