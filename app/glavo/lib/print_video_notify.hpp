#ifndef PRINT_VIDEO_NOTIFY_HPP
#define PRINT_VIDEO_NOTIFY_HPP

#include "msg_runtime.hpp"

#include <cstdint>
#include <string>

namespace upc::glavo {

/** Glavo → Camera 打印录像 IPC 通知（fire-and-forget，不阻塞换层） */
class PrintVideoNotify {
public:
    static PrintVideoNotify& instance();

    void set_print_h264_continuous(bool on);
    void set_timelapse_fps(uint32_t fps);
    void on_print_start(const std::string& job_path);
    void on_layer_snap(uint16_t layer_height_um);
    void on_print_finish();
    void on_print_abort();

private:
    PrintVideoNotify() = default;

    bool publish_to_camera(middleware::MsgType type,
                           const void* payload, size_t len);
    void on_session_end(middleware::MsgType type, const char* log_tag);
    void reset_session_state();

    bool     active_      = false;
    bool     print_h264_continuous_on_ = false;
    uint32_t layer_fps_   = 0; /**< 0=Media 侧默认 */
    uint32_t layer_index_ = 0;
    char     session_id_[32]{};
    char     job_name_[64]{};
};

} // namespace upc::glavo

#endif /* PRINT_VIDEO_NOTIFY_HPP */
