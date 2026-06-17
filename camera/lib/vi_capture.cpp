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
    frame_bytes_ =
        static_cast<size_t>(stride_y_) * height_ * 3u / 2u;

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

    /* preInit 第三参为文件名（非路径），init 第二参为目录；二者不可重复拼路径 */
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
    frame_bytes_ =
        static_cast<size_t>(stride_y_) * height_ * 3u / 2u;

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
            buffers_[i].start =
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
            buffers_[i].start =
                mmap(nullptr, buffers_[i].length, PROT_READ | PROT_WRITE,
                     MAP_SHARED, v4l2_fd_, buf.m.offset);
            const size_t y_size =
                static_cast<size_t>(stride_y_) * height_;
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

void ViCapture::log_capture_fail(const char* stage)
{
    static auto last_log = std::chrono::steady_clock::time_point{};
    const auto  now      = std::chrono::steady_clock::now();
    if(now - last_log < std::chrono::seconds(2)) {
        return;
    }
    last_log = now;
    UPC_LOG_CAMERA_SERVICE(Warn,
        "capture_frame fail stage=%s poll=%u dqbuf=%u copy=%u qbuf=%u "
        "dev_fd=%d mplane=%d",
        stage, cap_fail_poll_, cap_fail_dqbuf_, cap_fail_copy_, cap_fail_qbuf_,
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
}

bool ViCapture::copy_nv12_to(uint8_t* dst, size_t dst_size,
                             const uint8_t* src_y, const uint8_t* src_uv,
                             uint32_t src_stride_y, bool uv_separate_plane)
{
    if(!dst || !src_y) {
        return false;
    }

    const size_t compact =
        static_cast<size_t>(width_) * height_ * 3u / 2u;
    if(dst_size < compact) {
        return false;
    }

    const size_t y_plane = static_cast<size_t>(width_) * height_;
    const uint32_t uv_h  = height_ / 2u;

    if(!uv_separate_plane && src_stride_y == width_) {
        std::memcpy(dst, src_y, compact);
        return true;
    }

    for(uint32_t row = 0; row < height_; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * width_,
                    src_y + static_cast<size_t>(row) * src_stride_y,
                    width_);
    }

    if(!src_uv) {
        src_uv = src_y + static_cast<size_t>(src_stride_y) * height_;
    }

    uint8_t* dst_uv = dst + y_plane;
    for(uint32_t row = 0; row < uv_h; ++row) {
        std::memcpy(dst_uv + static_cast<size_t>(row) * width_,
                    src_uv + static_cast<size_t>(row) * src_stride_y,
                    width_);
    }
    return true;
}

bool ViCapture::capture_frame(uint8_t* dst, size_t dst_size, int timeout_ms)
{
    if(!open_ || v4l2_fd_ < 0) {
        return false;
    }

    pollfd pfd{};
    pfd.fd     = v4l2_fd_;
    pfd.events = POLLIN;
    const int pr = poll(&pfd, 1, timeout_ms);
    if(pr <= 0) {
        ++cap_fail_poll_;
        log_capture_fail(pr == 0 ? "poll_timeout" : "poll_error");
        return false;
    }

    const uint32_t buf_type = use_mplane_
                                  ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                  : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    v4l2_buffer buf{};
    v4l2_plane  planes[2]{};
    buf.type   = buf_type;
    buf.memory = V4L2_MEMORY_MMAP;
    if(use_mplane_) {
        buf.m.planes = planes;
        buf.length   = 2;
    }

    if(ioctl(v4l2_fd_, VIDIOC_DQBUF, &buf) < 0) {
        ++cap_fail_dqbuf_;
        log_capture_fail("dqbuf");
        return false;
    }

    const uint32_t idx = buf.index;
    if(idx >= v4l2_buf_count_ || !buffers_[idx].start) {
        if(use_mplane_ && buf.length >= 2) {
            buf.m.planes[0].bytesused = buf.m.planes[0].length;
            buf.m.planes[1].bytesused = buf.m.planes[1].length;
        }
        ioctl(v4l2_fd_, VIDIOC_QBUF, &buf);
        ++cap_fail_dqbuf_;
        log_capture_fail("dqbuf_bad_index");
        return false;
    }

    const uint8_t* src_y  = static_cast<const uint8_t*>(buffers_[idx].start);
    const uint8_t* src_uv = static_cast<const uint8_t*>(buffers_[idx].uv);
    const bool uv_sep =
        use_mplane_ && buffers_[idx].uv &&
        buffers_[idx].uv != buffers_[idx].start;
    if(!src_uv && !use_mplane_) {
        src_uv = src_y + static_cast<size_t>(stride_y_) * height_;
    }

    const bool ok = copy_nv12_to(dst, dst_size, src_y, src_uv, stride_y_, uv_sep);
    if(!ok) {
        ++cap_fail_copy_;
        log_capture_fail("copy_nv12");
    }

    if(use_mplane_ && buf.length >= 2) {
        if(buf.m.planes[0].bytesused == 0) {
            buf.m.planes[0].bytesused = buf.m.planes[0].length;
        }
        if(buf.m.planes[1].bytesused == 0) {
            buf.m.planes[1].bytesused = buf.m.planes[1].length;
        }
    }

    if(ioctl(v4l2_fd_, VIDIOC_QBUF, &buf) < 0) {
        ++cap_fail_qbuf_;
        UPC_LOG_CAMERA_SERVICE(Warn, "VIDIOC_QBUF failed: %s",
                               std::strerror(errno));
    }

    return ok;
}

} // namespace upc::camera
