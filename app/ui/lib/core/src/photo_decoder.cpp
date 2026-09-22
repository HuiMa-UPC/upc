#include "photo_decoder.hpp"

#include "png_decode_util.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ui {

struct PhotoDecoder::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> req_gen{0};

    /* 待解请求：只留最新一个（latest-wins） */
    bool pending = false;
    std::string pending_path;
    int32_t pending_box_w = 0;
    int32_t pending_box_h = 0;
    uint64_t pending_token = 0;
    uint64_t pending_gen = 0;

    /*
     * 双缓冲的分工：worker 解到 staging，主线程在 async_apply 里把 staging 换进
     * front，再更新 dsc。
     *
     * 与 VideoPlayer 的双缓冲翻页不同——那边一次 open 内几何恒定，只需翻转
     * dsc.data 指针；相册每张照片尺寸可能不同，dsc 的 w/h/stride 也要跟着改，
     * 而 LVGL 是在**绘制时刻**读 dsc 的。所以几何必须由主线程发布：async_apply
     * 跑在 LVGL 线程上，此刻不可能有绘制在进行，改 dsc 是安全的。
     */
    std::vector<uint8_t> front;
    std::vector<uint8_t> staging;
    int32_t staging_w = 0;
    int32_t staging_h = 0;
    lv_image_dsc_t dsc{};

    std::mutex ui_mtx;
    bool ui_pending = false;
    bool ui_failed = false;
    uint64_t ui_token = 0;
    uint64_t ui_gen = 0;
    bool async_scheduled = false;
    ReadyCb ready_cb;

    static void async_apply(void* user)
    {
        auto* self = static_cast<Impl*>(user);
        if(!self) {
            return;
        }
        ReadyCb cb;
        uint64_t token = 0;
        bool deliver = false;
        bool failed = false;
        {
            std::lock_guard<std::mutex> lk(self->ui_mtx);
            self->async_scheduled = false;
            const uint64_t latest = self->req_gen.load(std::memory_order_relaxed);
            if(!self->ui_pending || self->ui_gen != latest) {
                /* 已被更新的请求顶掉（或已 cancel）：丢弃，别把过期的图贴上去 */
                self->ui_pending = false;
                self->ui_failed = false;
                return;
            }
            failed = self->ui_failed;
            if(!failed) {
                if(self->staging.empty()) {
                    self->ui_pending = false;
                    return;
                }
                /* 先取尺寸再 swap：swap 之后 staging_* 描述的是已经换到 front
                 * 里的那批像素，容易看错，这里落成局部量。 */
                const int32_t w = self->staging_w;
                const int32_t h = self->staging_h;
                self->front.swap(self->staging);
                self->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
                self->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
                self->dsc.header.w = w;
                self->dsc.header.h = h;
                self->dsc.header.stride = static_cast<uint32_t>(w * 2);
                self->dsc.data = self->front.data();
                self->dsc.data_size = self->front.size();
            }
            token = self->ui_token;
            self->ui_pending = false;
            self->ui_failed = false;
            deliver = true;
            cb = self->ready_cb;
        }
        if(deliver && cb) {
            /* 成功时必须交出成员 dsc 的稳定地址，不能给栈上副本 */
            cb(failed ? nullptr : &self->dsc, token);
        }
    }

    /** 排一次主线程投递；调用方须已持有 ui_mtx */
    void schedule_async_locked()
    {
        /* 合并投递：快滑时不把 async 队列灌满，只保留一次待投递 */
        if(!async_scheduled) {
            async_scheduled = true;
            lv_async_call(async_apply, this);
        }
    }

    void publish(std::vector<uint8_t>& out, int32_t w, int32_t h,
                 uint64_t token, uint64_t gen)
    {
        if(gen != req_gen.load(std::memory_order_relaxed)) {
            return;
        }
        std::lock_guard<std::mutex> lk(ui_mtx);
        staging.swap(out);
        staging_w = w;
        staging_h = h;
        ui_pending = true;
        ui_failed = false;
        ui_token = token;
        ui_gen = gen;
        schedule_async_locked();
    }

    /**
     * 解不出来也要回一声：本类只吃 PNG（libpng），jpg/bmp 得让页面去走 LVGL 内置
     * 解码器。不回的话页面会一直停在「Loading…」。
     */
    void publish_failure(uint64_t token, uint64_t gen)
    {
        if(gen != req_gen.load(std::memory_order_relaxed)) {
            return;
        }
        std::lock_guard<std::mutex> lk(ui_mtx);
        ui_pending = true;
        ui_failed = true;
        ui_token = token;
        ui_gen = gen;
        schedule_async_locked();
    }

    static bool decode(const std::string& path, int32_t box_w, int32_t box_h,
                       std::vector<uint8_t>& out, int32_t& out_w, int32_t& out_h)
    {
        out_w = 0;
        out_h = 0;
        if(path.empty() || box_w <= 0 || box_h <= 0) {
            return false;
        }
        std::vector<uint8_t> rgba;
        uint32_t src_w = 0;
        uint32_t src_h = 0;
        if(!png_util::decode_rgba(path.c_str(), rgba, src_w, src_h)) {
            return false;
        }
        png_util::contain_fit(src_w, src_h, box_w, box_h, out_w, out_h);
        out.assign(static_cast<size_t>(out_w) * out_h * 2u, 0);
        /* 大图只解一次、要长期显示，用 box filter 换干净的降采样 */
        png_util::scale_to_rgb565_box(rgba.data(), src_w, src_h, out.data(),
                                      out_w, out_h);
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
            const std::string path = pending_path;
            const int32_t box_w = pending_box_w;
            const int32_t box_h = pending_box_h;
            const uint64_t token = pending_token;
            const uint64_t gen = pending_gen;
            pending = false;
            lk.unlock();

            if(gen == req_gen.load(std::memory_order_relaxed)) {
                std::vector<uint8_t> out;
                int32_t w = 0;
                int32_t h = 0;
                if(decode(path, box_w, box_h, out, w, h)) {
                    publish(out, w, h, token, gen);
                } else {
                    publish_failure(token, gen);
                }
            }

            lk.lock();
        }
    }
};

PhotoDecoder::PhotoDecoder()
{
    impl_ = new Impl();
    impl_->worker = std::thread([this] { impl_->worker_loop(); });
}

PhotoDecoder::~PhotoDecoder()
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

PhotoDecoder& PhotoDecoder::instance()
{
    static PhotoDecoder inst;
    return inst;
}

void PhotoDecoder::set_ready_cb(ReadyCb cb)
{
    if(!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lk(impl_->ui_mtx);
    impl_->ready_cb = std::move(cb);
}

void PhotoDecoder::request(const std::string& abs_path, int32_t box_w,
                           int32_t box_h, uint64_t token)
{
    if(!impl_ || abs_path.empty()) {
        return;
    }
    const uint64_t gen =
        impl_->req_gen.fetch_add(1, std::memory_order_relaxed) + 1;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    impl_->pending = true;
    impl_->pending_path = abs_path;
    impl_->pending_box_w = box_w;
    impl_->pending_box_h = box_h;
    impl_->pending_token = token;
    impl_->pending_gen = gen;
    impl_->cv.notify_all();
}

void PhotoDecoder::cancel()
{
    if(!impl_) {
        return;
    }
    /* 升世代号：在途解码算完也会因对不上 gen 而不投递 */
    impl_->req_gen.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(impl_->mtx);
        impl_->pending = false;
    }
    {
        std::lock_guard<std::mutex> ulk(impl_->ui_mtx);
        impl_->ui_pending = false;
        impl_->ui_failed = false;
        impl_->async_scheduled = false;
    }
    (void)lv_async_call_cancel(Impl::async_apply, impl_);
}

void PhotoDecoder::release()
{
    if(!impl_) {
        return;
    }
    cancel();
    std::lock_guard<std::mutex> ulk(impl_->ui_mtx);
    impl_->front.clear();
    impl_->front.shrink_to_fit();
    impl_->staging.clear();
    impl_->staging.shrink_to_fit();
    impl_->staging_w = 0;
    impl_->staging_h = 0;
    std::memset(&impl_->dsc, 0, sizeof(impl_->dsc));
}

} // namespace ui
