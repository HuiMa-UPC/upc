#include "vi_capture.hpp"

#include "device_config.hpp"
#include "rs_log.hpp"

#include <rkaiq/uAPI2/rk_aiq_user_api2_sysctl.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/videodev2.h>

namespace upc::camera {

namespace {

const char* env_or_null(const char* name)
{
    const char* v = std::getenv(name);
    return (v && v[0]) ? v : nullptr;
}

/** preInit 只接受文件名；若 env 给了绝对路径则拆出目录与 basename */
void normalize_iq_paths(const ViCaptureConfig& cfg, std::string& iq_dir,
                        std::string& iq_file)
{
    iq_dir  = cfg.iq_dir;
    iq_file = cfg.iq_file;
    if(iq_file.find('/') != std::string::npos) {
        const auto pos = iq_file.rfind('/');
        if(pos != std::string::npos) {
            if(iq_dir.empty() || iq_dir == CAMERA_IQ_DIR_DEFAULT) {
                iq_dir = iq_file.substr(0, pos);
            }
            iq_file = iq_file.substr(pos + 1);
        }
    }
}

const char* sensor_entity_for(const char* video_dev)
{
    if(const char* override_ent = env_or_null(CAMERA_ENV_SENSOR_ENTITY)) {
        return override_ent;
    }
    const char* sns = rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(video_dev);
    if(sns && sns[0] != '\0') {
        return sns;
    }
    return CAMERA_SENSOR_ENTITY_FALLBACK;
}

bool path_readable(const char* path)
{
    return path && path[0] && access(path, R_OK) == 0;
}

} // namespace

ViCapture::~ViCapture()
{
    close();
}

bool ViCapture::open(const ViCaptureConfig& cfg)
{
    if(open_) {
        close();
    }

    if(cfg.width == 0 || cfg.height == 0 || cfg.fps == 0 ||
       cfg.video_dev.empty() || cfg.iq_dir.empty() || cfg.iq_file.empty()) {
        UPC_LOG_CAMERA_SERVICE(Error, "ViCapture: invalid config");
        return false;
    }

    width_    = cfg.width;
    height_   = cfg.height;
    stride_y_ = cfg.width;
    frame_bytes_ = static_cast<size_t>(stride_y_) * height_ * 3u / 2u;

    if(!start_isp(cfg)) {
        return false;
    }
    if(!start_v4l2(cfg)) {
        stop_isp();
        return false;
    }

    open_ = true;
    UPC_LOG_CAMERA_SERVICE(Info,
        "ViCapture open dev=%s %ux%u@%u stride_y=%u iq=%s/%s",
        cfg.video_dev.c_str(), width_, height_, cfg.fps, stride_y_,
        cfg.iq_dir.c_str(), cfg.iq_file.c_str());
    return true;
}

void ViCapture::close()
{
    stop_v4l2();
    stop_isp();
    open_        = false;
    width_       = 0;
    height_      = 0;
    stride_y_    = 0;
    frame_bytes_ = 0;
}

bool ViCapture::start_isp(const ViCaptureConfig& cfg)
{
    std::string iq_dir;
    std::string iq_file;
    normalize_iq_paths(cfg, iq_dir, iq_file);

    char iq_path[512];
    std::snprintf(iq_path, sizeof(iq_path), "%s/%s",
                  iq_dir.c_str(), iq_file.c_str());
    if(!path_readable(iq_path)) {
        UPC_LOG_CAMERA_SERVICE(Error, "IQ file not readable: %s", iq_path);
        return false;
    }
    if(!path_readable(iq_dir.c_str())) {
        UPC_LOG_CAMERA_SERVICE(Error, "IQ dir not readable: %s",
                               iq_dir.c_str());
        return false;
    }

    const char* sns_ent = sensor_entity_for(cfg.video_dev.c_str());
    const auto  wdr     = RK_AIQ_WORKING_MODE_NORMAL;

    UPC_LOG_CAMERA_SERVICE(Info,
        "ISP preInit sns=%s iq_dir=%s iq_file=%s (full=%s)",
        sns_ent, iq_dir.c_str(), iq_file.c_str(), iq_path);

    XCamReturn pre =
        rk_aiq_uapi2_sysctl_preInit(sns_ent, wdr, iq_file.c_str());
    if(pre != XCAM_RETURN_NO_ERROR) {
        UPC_LOG_CAMERA_SERVICE(Error,
            "rk_aiq preInit failed (%d) sns=%s iq=%s", pre, sns_ent,
            iq_file.c_str());
        return false;
    }

    aiq_ctx_ = rk_aiq_uapi2_sysctl_init(sns_ent, iq_dir.c_str(),
                                         nullptr, nullptr);
    if(!aiq_ctx_) {
        UPC_LOG_CAMERA_SERVICE(Error, "rk_aiq init failed sns=%s dir=%s",
                               sns_ent, cfg.iq_dir.c_str());
        return false;
    }

    XCamReturn prep = rk_aiq_uapi2_sysctl_prepare(
        static_cast<const rk_aiq_sys_ctx_t*>(aiq_ctx_),
        width_, height_, wdr);
    if(prep != XCAM_RETURN_NO_ERROR) {
        UPC_LOG_CAMERA_SERVICE(Error, "rk_aiq prepare failed (%d)", prep);
        rk_aiq_uapi2_sysctl_deinit(static_cast<rk_aiq_sys_ctx_t*>(aiq_ctx_));
        aiq_ctx_ = nullptr;
        return false;
    }

    XCamReturn start = rk_aiq_uapi2_sysctl_start(
        static_cast<const rk_aiq_sys_ctx_t*>(aiq_ctx_));
    if(start != XCAM_RETURN_NO_ERROR) {
        UPC_LOG_CAMERA_SERVICE(Error, "rk_aiq start failed (%d)", start);
        rk_aiq_uapi2_sysctl_deinit(static_cast<rk_aiq_sys_ctx_t*>(aiq_ctx_));
        aiq_ctx_ = nullptr;
        return false;
    }

    isp_started_ = true;
    UPC_LOG_CAMERA_SERVICE(Info, "ISP started sns=%s iq=%s", sns_ent, iq_path);
    return true;
}

void ViCapture::stop_isp()
{
    if(!aiq_ctx_) {
        isp_started_ = false;
        return;
    }
    if(isp_started_) {
        rk_aiq_uapi2_sysctl_stop(static_cast<const rk_aiq_sys_ctx_t*>(aiq_ctx_),
                                 false);
        isp_started_ = false;
    }
    rk_aiq_uapi2_sysctl_deinit(static_cast<rk_aiq_sys_ctx_t*>(aiq_ctx_));
    aiq_ctx_ = nullptr;
}

bool ViCapture::start_v4l2(const ViCaptureConfig& cfg)
{
    v4l2_fd_ = ::open(cfg.video_dev.c_str(), O_RDWR | O_NONBLOCK);
    if(v4l2_fd_ < 0) {
        UPC_LOG_CAMERA_SERVICE(Error, "open %s failed: %s",
                               cfg.video_dev.c_str(), std::strerror(errno));
        return false;
    }

    v4l2_capability cap{};
    if(ioctl(v4l2_fd_, VIDIOC_QUERYCAP, &cap) == 0) {
        UPC_LOG_CAMERA_SERVICE(Info,
            "V4L2 cap dev=%s driver=%s card=%s mplane=%d capture=%d",
            cfg.video_dev.c_str(), cap.driver, cap.card,
            (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ? 1 : 0,
            (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ? 1 : 0);
        use_mplane_ = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
        if(!use_mplane_ &&
           (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
            UPC_LOG_CAMERA_SERVICE(Error, "device lacks capture: %s",
                                   cfg.video_dev.c_str());
            stop_v4l2();
            return false;
        }
    } else {
        use_mplane_ = true;
    }

    const uint32_t buf_type = use_mplane_
                                  ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                  : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    v4l2_format fmt{};
    fmt.type = buf_type;
    if(use_mplane_) {
        fmt.fmt.pix_mp.width        = width_;
        fmt.fmt.pix_mp.height       = height_;
        fmt.fmt.pix_mp.pixelformat  = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.field        = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes   = 2;
    } else {
        fmt.fmt.pix.width       = width_;
        fmt.fmt.pix.height      = height_;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field       = V4L2_FIELD_NONE;
    }

    if(ioctl(v4l2_fd_, VIDIOC_S_FMT, &fmt) < 0) {
        UPC_LOG_CAMERA_SERVICE(Error, "VIDIOC_S_FMT NV12 failed: %s",
                               std::strerror(errno));
        stop_v4l2();
        return false;
    }

    if(use_mplane_) {
        width_    = fmt.fmt.pix_mp.width;
        height_   = fmt.fmt.pix_mp.height;
        stride_y_ = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        if(stride_y_ == 0) {
            stride_y_ = width_;
        }
        UPC_LOG_CAMERA_SERVICE(Info,
            "V4L2 G_FMT mplane %ux%u NV12 planes=%u stride_y=%u sizeimage_y=%u",
            width_, height_, fmt.fmt.pix_mp.num_planes, stride_y_,
            fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    } else {
        width_    = fmt.fmt.pix.width;
        height_   = fmt.fmt.pix.height;
        stride_y_ = fmt.fmt.pix.bytesperline;
        if(stride_y_ == 0) {
            stride_y_ = width_;
        }
    }
    frame_bytes_ = static_cast<size_t>(stride_y_) * height_ * 3u / 2u;

    v4l2_streamparm parm{};
    parm.type = buf_type;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = cfg.fps;
    if(ioctl(v4l2_fd_, VIDIOC_S_PARM, &parm) < 0) {
        UPC_LOG_CAMERA_SERVICE(Warn, "VIDIOC_S_PARM fps=%u failed: %s",
                               cfg.fps, std::strerror(errno));
    }

    v4l2_requestbuffers req{};
    req.count  = cfg.buf_count > 0 ? cfg.buf_count : CAMERA_V4L2_BUF_COUNT;
    req.type   = buf_type;
    req.memory = V4L2_MEMORY_MMAP;
    if(ioctl(v4l2_fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        UPC_LOG_CAMERA_SERVICE(Error, "VIDIOC_REQBUFS failed: %s",
                               std::strerror(errno));
        stop_v4l2();
        return false;
    }

    v4l2_buf_count_ = req.count;
    buffers_        = new MappedBuffer[v4l2_buf_count_]{};

    for(uint32_t i = 0; i < v4l2_buf_count_; ++i) {
        v4l2_buffer buf{};
        v4l2_plane  planes[2]{};
        buf.type   = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if(use_mplane_) {
            buf.m.planes   = planes;
            buf.length     = 2;
            buf.m.planes[0].length = 0;
            buf.m.planes[1].length = 0;
        }

        if(ioctl(v4l2_fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            UPC_LOG_CAMERA_SERVICE(Error, "VIDIOC_QUERYBUF %u failed", i);
            stop_v4l2();
            return false;
        }

        if(use_mplane_) {
            const uint32_t nplanes = buf.length > 0 ? buf.length : 2u;
            buffers_[i].length = buf.m.planes[0].length;
            buffers_[i].start  =
                mmap(nullptr, buffers_[i].length, PROT_READ | PROT_WRITE,
                     MAP_SHARED, v4l2_fd_, buf.m.planes[0].m.mem_offset);
            if(nplanes >= 2 && buf.m.planes[1].length > 0) {
                buffers_[i].uv_len = buf.m.planes[1].length;
                buffers_[i].uv =
                    mmap(nullptr, buffers_[i].uv_len, PROT_READ | PROT_WRITE,
                         MAP_SHARED, v4l2_fd_, buf.m.planes[1].m.mem_offset);
            } else {
                buffers_[i].uv = static_cast<uint8_t*>(buffers_[i].start) +
                                 static_cast<size_t>(stride_y_) * height_;
                buffers_[i].uv_len = buffers_[i].length >
                                             static_cast<size_t>(stride_y_) *
                                                 height_
                                         ? buffers_[i].length -
                                               static_cast<size_t>(stride_y_) *
                                                   height_
                                         : 0;
            }
        } else {
            buffers_[i].length = buf.length;
            buffers_[i].start  =
                mmap(nullptr, buffers_[i].length, PROT_READ | PROT_WRITE,
                     MAP_SHARED, v4l2_fd_, buf.m.offset);
            const size_t y_size = static_cast<size_t>(stride_y_) * height_;
            buffers_[i].uv     = static_cast<uint8_t*>(buffers_[i].start) + y_size;
            buffers_[i].uv_len = buffers_[i].length > y_size
                                     ? buffers_[i].length - y_size
                                     : 0;
        }

        if(buffers_[i].start == MAP_FAILED) {
            UPC_LOG_CAMERA_SERVICE(Error, "mmap buffer %u failed", i);
            stop_v4l2();
            return false;
        }

        if(ioctl(v4l2_fd_, VIDIOC_QBUF, &buf) < 0) {
            UPC_LOG_CAMERA_SERVICE(Error, "VIDIOC_QBUF %u failed", i);
            stop_v4l2();
            return false;
        }
    }

    if(!export_dma_fds()) {
        UPC_LOG_CAMERA_SERVICE(Error, "VIDIOC_EXPBUF failed");
        stop_v4l2();
        return false;
    }

    if(ioctl(v4l2_fd_, VIDIOC_STREAMON, &buf_type) < 0) {
        UPC_LOG_CAMERA_SERVICE(Error, "VIDIOC_STREAMON failed: %s",
                               std::strerror(errno));
        stop_v4l2();
        return false;
    }

    UPC_LOG_CAMERA_SERVICE(Info,
        "V4L2 STREAMON ok dev=%s mplane=%d bufs=%u stride_y=%u frame_bytes=%zu",
        cfg.video_dev.c_str(), use_mplane_ ? 1 : 0, v4l2_buf_count_, stride_y_,
        frame_bytes_);

    return true;
}

bool ViCapture::export_dma_fds()
{
    dma_fds_.clear();
    dma_fds_.reserve(v4l2_buf_count_);

    const uint32_t buf_type = use_mplane_
                                  ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                  : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for(uint32_t i = 0; i < v4l2_buf_count_; ++i) {
        v4l2_exportbuffer exp{};
        exp.type  = buf_type;
        exp.index = i;
        if(ioctl(v4l2_fd_, VIDIOC_EXPBUF, &exp) < 0) {
            UPC_LOG_CAMERA_SERVICE(Error,
                "VIDIOC_EXPBUF buf=%u failed: %s", i, std::strerror(errno));
            for(int fd : dma_fds_) {
                if(fd >= 0) {
                    ::close(fd);
                }
            }
            dma_fds_.clear();
            return false;
        }
        dma_fds_.push_back(exp.fd);
    }

    UPC_LOG_CAMERA_SERVICE(Info, "V4L2 exported %u dma_buf fds",
                           v4l2_buf_count_);
    return true;
}

void ViCapture::log_capture_fail(const char* stage)
{
    static auto last_log = std::chrono::steady_clock::time_point{};
    const auto  now      = std::chrono::steady_clock::now();
    if(now - last_log < std::chrono::seconds(2)) {
        return;
    }
    last_log = now;
    UPC_LOG_CAMERA_SERVICE(Warn,
        "dequeue_frame fail stage=%s poll=%u dqbuf=%u qbuf=%u "
        "dev_fd=%d mplane=%d",
        stage, cap_fail_poll_, cap_fail_dqbuf_, cap_fail_qbuf_,
        v4l2_fd_, use_mplane_ ? 1 : 0);
}

void ViCapture::stop_v4l2()
{
    if(v4l2_fd_ >= 0) {
        const uint32_t buf_type = use_mplane_
                                      ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                      : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(v4l2_fd_, VIDIOC_STREAMOFF, &buf_type);
    }

    for(int fd : dma_fds_) {
        if(fd >= 0) {
            ::close(fd);
        }
    }
    dma_fds_.clear();

    if(buffers_) {
        for(uint32_t i = 0; i < v4l2_buf_count_; ++i) {
            if(buffers_[i].start && buffers_[i].start != MAP_FAILED) {
                munmap(buffers_[i].start, buffers_[i].length);
            }
            if(buffers_[i].uv && buffers_[i].uv != MAP_FAILED &&
               use_mplane_ &&
               buffers_[i].uv != buffers_[i].start) {
                munmap(buffers_[i].uv, buffers_[i].uv_len);
            }
        }
        delete[] buffers_;
        buffers_        = nullptr;
        v4l2_buf_count_ = 0;
    }

    if(v4l2_fd_ >= 0) {
        ::close(v4l2_fd_);
        v4l2_fd_ = -1;
    }

    /*
     * 重开流后驱动的 sequence 从 0 重新计数。不清这个基准的话，跳号判据
     * （新号 > 旧号+1）在追上旧号之前恒为假，drv_drop 会一直是 0——不是没丢帧，
     * 是根本没在测。timestamp_monotonic 是驱动的固有属性，故意保留不清。
     */
    lat_have_sequence_ = false;
    lat_last_sequence_ = 0;
}

std::optional<uint32_t> ViCapture::dequeue_frame(int timeout_ms)
{
    if(!open_ || v4l2_fd_ < 0) {
        return std::nullopt;
    }

    pollfd pfd{};
    pfd.fd     = v4l2_fd_;
    pfd.events = POLLIN;
    const int pr = poll(&pfd, 1, timeout_ms);
    if(pr <= 0) {
        ++cap_fail_poll_;
        log_capture_fail(pr == 0 ? "poll_timeout" : "poll_error");
        return std::nullopt;
    }

    const uint32_t buf_type = use_mplane_
                                  ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                  : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    /*
     * 排空到最新帧。
     *
     * V4L2 是 FIFO：一次 DQBUF 拿到的是队列里最老的一帧。而采集线程按固定
     * 帧周期"取一还一"，周期里 sleep_for 的过冲让它比 sensor 慢约 0.1ms/帧，
     * 这点差额只能累积、永远排不掉，最终把画面延迟顶到"缓冲数 × 帧周期"并
     * 让驱动开始丢帧（实测 40s 内从 20ms 爬到 141ms）。
     *
     * 这里在首帧之后继续非阻塞 DQBUF 直到 EAGAIN，把过期帧立即 QBUF 还回去，
     * 只交付最新一帧，积压因此变成可自愈的。fd 是 O_NONBLOCK 打开的（见
     * start_v4l2），所以队列空时 DQBUF 立即返回 EAGAIN，不会阻塞。
     *
     * 代价：消费者卡顿期间过期帧被丢弃而不是排队，打印录像在卡顿瞬间会少几帧。
     * 稳态 30fps 进 30fps 出时每轮通常一帧都排不出来，影响可忽略。
     */
    auto dqbuf_once = [&](v4l2_buffer& buf, v4l2_plane* planes) -> bool {
        buf        = {};
        buf.type   = buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        if(use_mplane_) {
            buf.m.planes = planes;
            buf.length   = 2;
        }
        return ioctl(v4l2_fd_, VIDIOC_DQBUF, &buf) >= 0;
    };

    v4l2_buffer buf{};
    v4l2_plane  planes[2]{};
    if(!dqbuf_once(buf, planes)) {
        ++cap_fail_dqbuf_;
        log_capture_fail("dqbuf");
        return std::nullopt;
    }

    /*
     * 只留下统计要用的两个标量，不整体搬 v4l2_buffer：它的 m.planes 指向
     * 循环内的局部数组，拷出来就是悬垂指针。
     */
    uint32_t idx   = buf.index;
    uint32_t flags = buf.flags;
    int64_t  ts_us = static_cast<int64_t>(buf.timestamp.tv_sec) * 1000000 +
                     buf.timestamp.tv_usec;
    note_dequeued_sequence(buf.sequence);

    /*
     * 上限取缓冲总数：还回去的 buffer 驱动可以立刻再填，理论上存在"边排边填"
     * 的活锁。30fps 对上微秒级的循环不会发生，但这是被外部设备驱动的循环，
     * 给个硬上限保证一定收敛。
     */
    for(uint32_t i = 0; i < v4l2_buf_count_; ++i) {
        v4l2_buffer next{};
        v4l2_plane  next_planes[2]{};
        if(!dqbuf_once(next, next_planes)) {
            /* EAGAIN 是队列已空的正常收尾，不是采集失败，不能计进 dqbuf 失败数。 */
            if(errno != EAGAIN && errno != EWOULDBLOCK) {
                ++cap_fail_dqbuf_;
                log_capture_fail("dqbuf_drain");
            }
            break;
        }
        /* 上一帧已过期：立刻还给驱动，只留最新的。 */
        queue_frame(idx);
        idx   = next.index;
        flags = next.flags;
        ts_us = static_cast<int64_t>(next.timestamp.tv_sec) * 1000000 +
                next.timestamp.tv_usec;
        /*
         * 排空掉的帧也要过一遍 sequence 跟踪，否则它在帧号上留下的空缺会被
         * 当成驱动丢帧记进 drv_drop——那是我们主动丢的，已经记在 drained 里了。
         */
        note_dequeued_sequence(next.sequence);
        ++lat_stats_.drained;
    }

    if(idx >= v4l2_buf_count_ || !buffers_[idx].start) {
        UPC_LOG_CAMERA_SERVICE(Warn, "dequeue_frame bad index %u", idx);
        return std::nullopt;
    }

    /* 滞留时间只看真正交付的那一帧：画面延迟取决于它，不是被丢掉的过期帧。 */
    accumulate_delivered_latency(flags, ts_us);
    return idx;
}

/**
 * 记录一帧的驱动侧帧号，累积跳号数（即驱动因无空闲 buffer 丢掉的帧数）。
 *
 * @param sequence v4l2_buffer::sequence，驱动侧连续帧号
 *
 * **每一帧 DQBUF 出来都要调用，包括被排空丢弃的**。否则我们主动丢掉的帧会在
 * 帧号上留下空缺，被误记成驱动丢帧——两者要分开：drained 是拿延迟换新鲜度
 * （正常），seq_gap 是驱动来不及（异常）。
 */
void ViCapture::note_dequeued_sequence(uint32_t sequence)
{
    if(lat_have_sequence_ && sequence > lat_last_sequence_ + 1u) {
        lat_stats_.seq_gap += sequence - lat_last_sequence_ - 1u;
    }
    lat_last_sequence_ = sequence;
    lat_have_sequence_ = true;
}

/**
 * 累积交付帧在驱动队列里的滞留时间。
 *
 * @param flags v4l2_buffer::flags，用于校验时间戳时钟域
 * @param ts_us v4l2_buffer::timestamp 折算的微秒值，即驱动填完该帧的时刻
 *
 * ts_us 与当前单调时钟之差即"这一帧在驱动队列里躺了多久"。稳定在一帧周期
 * 内说明无积压；持续爬升说明排空没生效或另有阻塞点。只对交付帧统计——
 * 被排空丢掉的帧不上屏，它们的滞留时间不构成画面延迟。
 */
void ViCapture::accumulate_delivered_latency(uint32_t flags, int64_t ts_us)
{
    const uint32_t ts_type = flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
    lat_stats_.timestamp_monotonic =
        (ts_type == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC);

    if(!lat_stats_.timestamp_monotonic) {
        return;
    }
    timespec now{};
    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return;
    }
    const int64_t now_us =
        static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
    const int64_t age_us = now_us - ts_us;
    /* 负值只可能来自时钟域不一致，按无效样本丢弃。 */
    if(age_us < 0) {
        return;
    }
    ++lat_stats_.samples;
    lat_stats_.latency_us_sum += static_cast<uint64_t>(age_us);
    if(static_cast<uint64_t>(age_us) > lat_stats_.latency_us_max) {
        lat_stats_.latency_us_max = static_cast<uint64_t>(age_us);
    }
}

ViCaptureLatencyStats ViCapture::take_latency_stats()
{
    const ViCaptureLatencyStats out = lat_stats_;
    /* 只清累积量，保留时间戳类型这一常量结论。 */
    lat_stats_.samples        = 0;
    lat_stats_.latency_us_sum = 0;
    lat_stats_.latency_us_max = 0;
    lat_stats_.seq_gap        = 0;
    lat_stats_.drained        = 0;
    return out;
}

bool ViCapture::queue_frame(uint32_t index)
{
    if(!open_ || v4l2_fd_ < 0 || index >= v4l2_buf_count_) {
        return false;
    }

    const uint32_t buf_type = use_mplane_
                                  ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                  : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    v4l2_buffer buf{};
    v4l2_plane  planes[2]{};
    buf.type   = buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = index;
    if(use_mplane_) {
        buf.m.planes = planes;
        buf.length   = 2;
        buf.m.planes[0].bytesused = buf.m.planes[0].length;
        buf.m.planes[1].bytesused = buf.m.planes[1].length;
    } else {
        buf.bytesused = buffers_[index].length;
    }

    if(ioctl(v4l2_fd_, VIDIOC_QBUF, &buf) < 0) {
        ++cap_fail_qbuf_;
        UPC_LOG_CAMERA_SERVICE(Warn, "VIDIOC_QBUF %u failed: %s",
                               index, std::strerror(errno));
        return false;
    }
    return true;
}

const uint8_t* ViCapture::buffer_y(uint32_t idx) const
{
    if(idx >= v4l2_buf_count_ || !buffers_) {
        return nullptr;
    }
    return static_cast<const uint8_t*>(buffers_[idx].start);
}

const uint8_t* ViCapture::buffer_uv(uint32_t idx) const
{
    if(idx >= v4l2_buf_count_ || !buffers_) {
        return nullptr;
    }
    return static_cast<const uint8_t*>(buffers_[idx].uv);
}

} // namespace upc::camera
