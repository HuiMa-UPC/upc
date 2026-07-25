#ifndef UI_VIDEO_PLAYER_HPP
#define UI_VIDEO_PLAYER_HPP

#include "lvgl_wrap.hpp"

#include <cstdint>
#include <functional>

namespace ui {

class VideoPlayer {
public:
    using ProgressCb = std::function<void(int64_t current_ms, int64_t duration_ms)>;
    using EndedCb = std::function<void()>;
    using PreviewCb = std::function<void(const lv_image_dsc_t* image, int64_t position_ms)>;

    static VideoPlayer& instance();

    bool open(lv_obj_t* image_target, const char* path);
    void close();

    bool play();
    void pause();
    bool seek(int64_t position_ms);
    void request_preview(int64_t position_ms);

    void set_progress_cb(ProgressCb cb);
    void set_ended_cb(EndedCb cb);
    void set_preview_cb(PreviewCb cb);

    [[nodiscard]] bool is_open() const;
    [[nodiscard]] bool is_playing() const;
    [[nodiscard]] int64_t duration_ms() const;
    [[nodiscard]] int64_t position_ms() const;

private:
    VideoPlayer();
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace ui

#endif /* UI_VIDEO_PLAYER_HPP */
