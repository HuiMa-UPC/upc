#ifndef UI_PNG_SCRUB_PREVIEW_HPP
#define UI_PNG_SCRUB_PREVIEW_HPP

#include "lvgl_wrap.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace ui {

/**
 * PNG 序列拖动预览（对齐 VideoPlayer::request_preview）：
 * - 后台线程解码并缩放到小图 RGB565
 * - latest-wins：只投递最新请求
 * - 主线程仅贴预览框，不碰主画面
 */
class PngScrubPreview {
public:
    using PreviewCb =
        std::function<void(const lv_image_dsc_t* image, size_t frame_index)>;

    static PngScrubPreview& instance();

    void set_target_size(int32_t w, int32_t h);
    void set_preview_cb(PreviewCb cb);

    /** @param abs_path 真实文件系统路径（无 LVGL A: 前缀） */
    void request(size_t frame_index, const std::string& abs_path);
    void set_scrubbing(bool scrubbing);
    void cancel();

private:
    PngScrubPreview();
    ~PngScrubPreview();
    PngScrubPreview(const PngScrubPreview&) = delete;
    PngScrubPreview& operator=(const PngScrubPreview&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace ui

#endif /* UI_PNG_SCRUB_PREVIEW_HPP */
