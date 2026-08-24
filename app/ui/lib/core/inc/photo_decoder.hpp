#ifndef UI_PHOTO_DECODER_HPP
#define UI_PHOTO_DECODER_HPP

#include "lvgl_wrap.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace ui {

/**
 * 相册大图后台解码（形状对齐 VideoPlayer：后台线程 + latest-wins + lv_async_call）。
 *
 * 为什么不能在主线程直接 lv_image_set_src(原图路径)：照片是 1080×1280 PNG，
 * LV_COLOR_DEPTH 为 32，lodepng 会在 LVGL 线程内同步解出约 5.5MB 的 ARGB8888；
 * 而本工程 LV_CACHE_DEF_SIZE = 0（图片缓存关闭），**每次重绘都要重解一遍**。
 *
 * 所以这里在后台把图解好并等比缩到显示尺寸的 RGB565，主线程只贴一个内存 dsc：
 * 既不阻塞 UI，也让绘制变成 1:1 blit。
 */
class PhotoDecoder {
public:
    /** @param token request() 时传入的原样回传值（页面用它判断这张是否还是当前该显示的） */
    using ReadyCb = std::function<void(const lv_image_dsc_t* image, uint64_t token)>;

    static PhotoDecoder& instance();

    void set_ready_cb(ReadyCb cb);

    /**
     * 请求解码一张图，等比贴合进 box（不放大；放大交给 LVGL 的 CONTAIN）。
     *
     * 只保留最新一次请求：连续快滑时中途那些直接丢掉、不排队，避免手停下来之后
     * 还要把一路划过的照片逐张解完。
     */
    void request(const std::string& abs_path, int32_t box_w, int32_t box_h,
                 uint64_t token);

    /** 丢弃在途请求与尚未投递的回调；保留缓冲以便下一张复用（切图时用） */
    void cancel();

    /**
     * cancel + 释放解码缓冲（离页时用）。
     *
     * 页面对象由 PageManager 惰性创建后**永不销毁**，不在离页时释放的话这几 MB
     * 会常驻到进程结束。调用前必须先把指向本类 dsc 的 lv_image 清空
     * （lv_image_set_src(img, nullptr)），否则控件下一帧会读到已释放的内存。
     */
    void release();

private:
    PhotoDecoder();
    ~PhotoDecoder();
    PhotoDecoder(const PhotoDecoder&) = delete;
    PhotoDecoder& operator=(const PhotoDecoder&) = delete;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace ui

#endif /* UI_PHOTO_DECODER_HPP */
