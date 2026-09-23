#include "png_scrub_preview.hpp"

#include "png_decode_util.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

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

    bool decode_to_preview(const std::string& path, int32_t dst_w, int32_t dst_h)
    {
        if(path.empty() || dst_w <= 0 || dst_h <= 0) {
            return false;
        }
        std::vector<uint8_t> rgba;
        uint32_t src_w = 0;
        uint32_t src_h = 0;
        if(!png_util::decode_rgba(path.c_str(), rgba, src_w, src_h)) {
            return false;
        }
        const size_t need = static_cast<size_t>(dst_w) * dst_h * 2u;
        if(rgb565.size() != need) {
            rgb565.assign(need, 0);
        }
        /* 预览是一闪而过的小图，取最快的点采样；相册大图那边才用 box filter。 */
        png_util::scale_to_rgb565_point(rgba.data(), src_w, src_h, rgb565.data(),
                                        dst_w, dst_h);
        dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        dsc.header.w = dst_w;
        dsc.header.h = dst_h;
        dsc.header.stride = static_cast<uint32_t>(dst_w * 2);
        dsc.data = rgb565.data();
        dsc.data_size = rgb565.size();
        return true;
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
    {
        std::lock_guard<std::mutex> ulk(impl_->ui_mtx);
        impl_->ui_pending = false;
        impl_->preview_cb = nullptr;
    }
    (void)lv_async_call_cancel(Impl::async_apply, impl_);
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
    if(impl_) {
        (void)lv_async_call_cancel(Impl::async_apply, impl_);
    }
}

} // namespace ui
