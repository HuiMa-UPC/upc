#include "png_scrub_preview.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(UPC_UI_HAS_LIBPNG) && UPC_UI_HAS_LIBPNG
#include <png.h>
#endif

namespace ui {

struct PngScrubPreview::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> scrubbing{false};
    std::atomic<uint64_t> req_gen{0};

    int32_t target_w = 160;
    int32_t target_h = 128;

    bool pending = false;
    size_t pending_index = 0;
    std::string pending_path;
    uint64_t pending_gen = 0;

    std::vector<uint8_t> rgb565;
    lv_image_dsc_t dsc{};

    std::mutex ui_mtx;
    bool ui_pending = false;
    size_t ui_index = 0;
    uint64_t ui_gen = 0;

    PreviewCb preview_cb;

    static void async_apply(void* user)
    {
        auto* self = static_cast<Impl*>(user);
        if(!self) {
            return;
        }
        PreviewCb cb;
        const lv_image_dsc_t* image = nullptr;
        size_t index = 0;
        {
            std::lock_guard<std::mutex> lk(self->ui_mtx);
            if(!self->ui_pending || !self->dsc.data) {
                self->ui_pending = false;
                return;
            }
            const uint64_t latest = self->req_gen.load(std::memory_order_relaxed);
            if(self->ui_gen != latest ||
               !self->scrubbing.load(std::memory_order_relaxed)) {
                self->ui_pending = false;
                return;
            }
            cb = self->preview_cb;
            image = &self->dsc;
            index = self->ui_index;
            self->ui_pending = false;
        }
        if(cb && image) {
            cb(image, index);
        }
    }

    void notify_ui(size_t index, uint64_t gen)
    {
        if(gen != req_gen.load(std::memory_order_relaxed) ||
           !scrubbing.load(std::memory_order_relaxed)) {
            return;
        }
        std::lock_guard<std::mutex> lk(ui_mtx);
        ui_pending = true;
        ui_index = index;
        ui_gen = gen;
        lv_async_call(async_apply, this);
    }

#if defined(UPC_UI_HAS_LIBPNG) && UPC_UI_HAS_LIBPNG
    static bool decode_png_rgba(const char* path,
                                std::vector<uint8_t>& rgba,
                                uint32_t& w,
                                uint32_t& h)
    {
        FILE* fp = std::fopen(path, "rb");
        if(!fp) {
            return false;
        }
        png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
                                                 nullptr, nullptr, nullptr);
        if(!png) {
            std::fclose(fp);
            return false;
        }
        png_infop info = png_create_info_struct(png);
        if(!info) {
            png_destroy_read_struct(&png, nullptr, nullptr);
            std::fclose(fp);
            return false;
        }
        if(setjmp(png_jmpbuf(png))) {
            png_destroy_read_struct(&png, &info, nullptr);
            std::fclose(fp);
            return false;
        }
        png_init_io(png, fp);
        png_read_info(png, info);
        w = png_get_image_width(png, info);
        h = png_get_image_height(png, info);
        const png_byte color_type = png_get_color_type(png, info);
        const png_byte bit_depth = png_get_bit_depth(png, info);
        if(bit_depth == 16) {
            png_set_strip_16(png);
        }
        if(color_type == PNG_COLOR_TYPE_PALETTE) {
            png_set_palette_to_rgb(png);
        }
        if(color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
            png_set_expand_gray_1_2_4_to_8(png);
        }
        if(png_get_valid(png, info, PNG_INFO_tRNS)) {
            png_set_tRNS_to_alpha(png);
        }
        if(color_type == PNG_COLOR_TYPE_RGB ||
           color_type == PNG_COLOR_TYPE_GRAY ||
           color_type == PNG_COLOR_TYPE_PALETTE) {
            png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
        }
        if(color_type == PNG_COLOR_TYPE_GRAY ||
           color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
            png_set_gray_to_rgb(png);
        }
        png_read_update_info(png, info);
        if(w == 0 || h == 0 || w > 8192 || h > 8192) {
            png_destroy_read_struct(&png, &info, nullptr);
            std::fclose(fp);
            return false;
        }
        rgba.assign(static_cast<size_t>(w) * h * 4u, 0);
        std::vector<png_bytep> rows(h);
        for(uint32_t y = 0; y < h; ++y) {
            rows[y] = rgba.data() + static_cast<size_t>(y) * w * 4u;
        }
        png_read_image(png, rows.data());
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return true;
    }
#endif

    static void scale_rgba_to_rgb565(const uint8_t* rgba,
                                     uint32_t src_w,
                                     uint32_t src_h,
                                     uint8_t* out,
                                     int32_t dst_w,
                                     int32_t dst_h)
    {
        for(int32_t y = 0; y < dst_h; ++y) {
            const uint32_t sy = static_cast<uint32_t>(y) * src_h /
                               static_cast<uint32_t>(dst_h);
            for(int32_t x = 0; x < dst_w; ++x) {
                const uint32_t sx = static_cast<uint32_t>(x) * src_w /
                                   static_cast<uint32_t>(dst_w);
                const uint8_t* p =
                    rgba + (static_cast<size_t>(sy) * src_w + sx) * 4u;
                const uint16_t r = p[0] >> 3;
                const uint16_t g = p[1] >> 2;
                const uint16_t b = p[2] >> 3;
                const uint16_t pix =
                    static_cast<uint16_t>((r << 11) | (g << 5) | b);
                const size_t off =
                    (static_cast<size_t>(y) * dst_w + x) * 2u;
                out[off] = static_cast<uint8_t>(pix & 0xFFu);
                out[off + 1] = static_cast<uint8_t>((pix >> 8) & 0xFFu);
            }
        }
    }

    bool decode_to_preview(const std::string& path, int32_t dst_w, int32_t dst_h)
    {
        if(path.empty() || dst_w <= 0 || dst_h <= 0) {
            return false;
        }
#if defined(UPC_UI_HAS_LIBPNG) && UPC_UI_HAS_LIBPNG
        std::vector<uint8_t> rgba;
        uint32_t src_w = 0;
        uint32_t src_h = 0;
        if(!decode_png_rgba(path.c_str(), rgba, src_w, src_h)) {
            return false;
        }
        const size_t need = static_cast<size_t>(dst_w) * dst_h * 2u;
        if(rgb565.size() != need) {
            rgb565.assign(need, 0);
        }
        scale_rgba_to_rgb565(rgba.data(), src_w, src_h, rgb565.data(),
                             dst_w, dst_h);
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc.header.w = dst_w;
        dsc.header.h = dst_h;
        dsc.header.stride = static_cast<uint32_t>(dst_w * 2);
        dsc.data = rgb565.data();
        dsc.data_size = rgb565.size();
        return true;
#else
        (void)path;
        (void)dst_w;
        (void)dst_h;
        return false;
#endif
    }

    void worker_loop()
    {
        std::unique_lock<std::mutex> lk(mtx);
        while(!stop.load(std::memory_order_relaxed)) {
            cv.wait(lk, [this] {
                return stop.load(std::memory_order_relaxed) || pending;
            });
            if(stop.load(std::memory_order_relaxed)) {
                break;
            }
            const size_t index = pending_index;
            const std::string path = pending_path;
            const uint64_t gen = pending_gen;
            const int32_t tw = target_w;
            const int32_t th = target_h;
            pending = false;
            lk.unlock();

            if(gen == req_gen.load(std::memory_order_relaxed) &&
               scrubbing.load(std::memory_order_relaxed) &&
               decode_to_preview(path, tw, th)) {
                notify_ui(index, gen);
            }

            lk.lock();
        }
    }
};

PngScrubPreview::PngScrubPreview()
{
    impl_ = new Impl();
    impl_->worker = std::thread([this] { impl_->worker_loop(); });
}

PngScrubPreview::~PngScrubPreview()
{
    if(!impl_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->stop = true;
        impl_->cv.notify_all();
    }
    if(impl_->worker.joinable()) {
        impl_->worker.join();
    }
    delete impl_;
    impl_ = nullptr;
}

PngScrubPreview& PngScrubPreview::instance()
{
    static PngScrubPreview inst;
    return inst;
}

void PngScrubPreview::set_target_size(int32_t w, int32_t h)
{
    if(!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->target_w = std::max<int32_t>(1, w);
    impl_->target_h = std::max<int32_t>(1, h);
}

void PngScrubPreview::set_preview_cb(PreviewCb cb)
{
    if(!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->ui_mtx);
    impl_->preview_cb = std::move(cb);
}

void PngScrubPreview::request(size_t frame_index, const std::string& abs_path)
{
    if(!impl_ || abs_path.empty()) {
        return;
    }
    const uint64_t gen =
        impl_->req_gen.fetch_add(1, std::memory_order_relaxed) + 1;
    impl_->scrubbing.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->pending = true;
    impl_->pending_index = frame_index;
    impl_->pending_path = abs_path;
    impl_->pending_gen = gen;
    impl_->cv.notify_all();
}

void PngScrubPreview::set_scrubbing(bool scrubbing)
{
    if(!impl_) {
        return;
    }
    impl_->scrubbing.store(scrubbing, std::memory_order_relaxed);
    if(!scrubbing) {
        impl_->req_gen.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->pending = false;
        std::lock_guard<std::mutex> ulk(impl_->ui_mtx);
        impl_->ui_pending = false;
    }
}

void PngScrubPreview::cancel()
{
    set_scrubbing(false);
}

} // namespace ui
