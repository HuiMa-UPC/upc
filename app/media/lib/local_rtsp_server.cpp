#include "local_rtsp_server.hpp"

#include "device_config.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>

#if defined(UPC_MEDIA_HAS_LIVE555) && UPC_MEDIA_HAS_LIVE555
#include <liveMedia.hh>
#include <BasicUsageEnvironment.hh>
#endif

namespace upc::media {

namespace {

#if UPC_MEDIA_LOG_STREAM_STAT
/**
 * 墙钟微秒，仅供心跳日志计时。
 * 帧的 presentation time 由编码器在采帧时打好后透传，不在本文件取时刻。
 */
int64_t now_us()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}
#endif

/** WiFi 接口名；与 `wifi_manager` 的 `UPC_WIFI_IFACE` 约定保持一致。 */
const char* wifi_iface_name()
{
    const char* env = std::getenv("UPC_WIFI_IFACE");
    return (env && env[0]) ? env : "wlan0";
}

bool name_starts_with(const char* name, const char* prefix)
{
    return std::strncmp(name, prefix, std::strlen(prefix)) == 0;
}

/**
 * 明确不可能用于局域网拉流的虚拟网卡。
 * `dummy0` 常带 169.254 地址且接口序号排在真实网卡之前，是踩坑重灾区。
 */
bool is_virtual_iface(const char* name)
{
    static const char* const kVirtualPrefixes[] = {
        "dummy", "docker", "veth", "br-", "virbr", "tun", "tap", "sit", "ip6tnl"
    };
    for(const char* prefix : kVirtualPrefixes) {
        if(name_starts_with(name, prefix)) {
            return true;
        }
    }
    return false;
}

/** 169.254.0.0/16：没拿到 DHCP 时的自动配置地址，局域网内对端不可达。 */
bool is_link_local(const struct sockaddr_in* addr)
{
    return (ntohl(addr->sin_addr.s_addr) & 0xFFFF0000u) == 0xA9FE0000u;
}

/** 接口优先级：设备主力联网方式是 WiFi，客户端多半也在同一 WiFi 下。 */
int iface_rank(const char* name)
{
    if(std::strcmp(name, wifi_iface_name()) == 0) return 100;
    if(name_starts_with(name, "wlan") || name_starts_with(name, "wlp")) return 90;
    if(name_starts_with(name, "eth") || name_starts_with(name, "en")) return 80;
    if(name_starts_with(name, "usb")) return 40;   /**< USB RNDIS 一般只连开发机 */
    return 50;
}

} // namespace

#if defined(UPC_MEDIA_HAS_LIVE555) && UPC_MEDIA_HAS_LIVE555

namespace {

/** SDP 中声明的估计码率（kbps）；仅供客户端参考。 */
constexpr unsigned kEstBitrateKbps = CAMERA_PRINT_H264_BITRATE / 1000u;

/**
 * 把 live555 的诊断输出接进 rs_log。
 *
 * 库内部靠 `envir() << ...` 报错（如 MultiFramedRTPSink 的
 * "input frame data was too large for our buffer size"），默认落 stderr，
 * 不进 topic 日志——而这类库内告警恰恰是排查花屏/丢帧的关键线索。
 *
 * 一条消息会分多次 `<<` 进来，故按行缓冲、遇 '\n' 才成条输出。
 * 所有调用都在 live555 事件循环线程，故 line_ 无需加锁。
 */
class LogUsageEnvironment : public BasicUsageEnvironment {
public:
    static LogUsageEnvironment* createNew(TaskScheduler& scheduler)
    {
        return new LogUsageEnvironment(scheduler);
    }

    UsageEnvironment& operator<<(char const* str) override
    {
        return append(str);
    }
    UsageEnvironment& operator<<(int i) override
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", i);
        return append(buf);
    }
    UsageEnvironment& operator<<(unsigned u) override
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u", u);
        return append(buf);
    }
    UsageEnvironment& operator<<(double d) override
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%f", d);
        return append(buf);
    }
    UsageEnvironment& operator<<(void* p) override
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%p", p);
        return append(buf);
    }

protected:
    explicit LogUsageEnvironment(TaskScheduler& scheduler)
        : BasicUsageEnvironment(scheduler)
    {
    }

    /** 只由 reclaim() 删除；退出前把残留的半行冲出去。 */
    ~LogUsageEnvironment() override
    {
        flush_line();
    }

private:
    UsageEnvironment& append(const char* text)
    {
        if(!text) {
            return *this;
        }
        for(const char* p = text; *p != '\0'; ++p) {
            if(*p == '\n') {
                flush_line();
            } else if(line_.size() < kMaxLineChars) {
                line_.push_back(*p);
            }
        }
        return *this;
    }

    void flush_line()
    {
        if(line_.empty()) {
            return;
        }
        /* 带 local_rtsp 前缀，保证与本模块日志同归 topic_media_rtsp.log */
        UPC_LOG_MEDIA_SERVICE(Warn, "local_rtsp live555: %s", line_.c_str());
        line_.clear();
    }

    static constexpr size_t kMaxLineChars = 512;
    std::string line_;
};

/**
 * live555 帧源：从 LocalRtspServer 的 NAL 队列取数据。
 * 全部方法都在事件循环线程执行。
 */
class H264FramedSource : public FramedSource {
public:
    static H264FramedSource* createNew(UsageEnvironment& env, LocalRtspServer* owner)
    {
        return new H264FramedSource(env, owner);
    }

    /** 若下游正在等数据则投递一个 NAL；队列空则静默返回，等下次 trigger。 */
    void deliver_now()
    {
        if(!isCurrentlyAwaitingData()) {
            return;
        }
        LocalRtspServer::NalUnit nal;
        if(!owner_->pop_nal(nal)) {
            return;
        }
        if(nal.data.size() > fMaxSize) {
            fFrameSize = fMaxSize;
            fNumTruncatedBytes = static_cast<unsigned>(nal.data.size()) - fMaxSize;
        } else {
            fFrameSize = static_cast<unsigned>(nal.data.size());
            fNumTruncatedBytes = 0;
        }
        std::memcpy(fTo, nal.data.data(), fFrameSize);
        fPresentationTime.tv_sec = static_cast<time_t>(nal.pts_us / 1000000);
        fPresentationTime.tv_usec = static_cast<suseconds_t>(nal.pts_us % 1000000);
        fDurationInMicroseconds = 0;
        FramedSource::afterGetting(this);
    }

protected:
    H264FramedSource(UsageEnvironment& env, LocalRtspServer* owner)
        : FramedSource(env), owner_(owner)
    {
        owner_->attach_source(this);
    }

    ~H264FramedSource() override
    {
        owner_->detach_source(this);
    }

    void doGetNextFrame() override
    {
        deliver_now();
    }

private:
    LocalRtspServer* owner_;
};

/**
 * 单路 H.264 子会话。
 * reuseFirstSource=True：多个客户端共用同一个 source，不会各开一路编码。
 */
class LiveH264Subsession : public OnDemandServerMediaSubsession {
public:
    static LiveH264Subsession* createNew(UsageEnvironment& env, LocalRtspServer* owner)
    {
        return new LiveH264Subsession(env, owner);
    }

protected:
    LiveH264Subsession(UsageEnvironment& env, LocalRtspServer* owner)
        : OnDemandServerMediaSubsession(env, True), owner_(owner)
    {
    }

    FramedSource* createNewStreamSource(unsigned /*clientSessionId*/,
                                        unsigned& estBitrate) override
    {
        estBitrate = kEstBitrateKbps;
        H264FramedSource* src = H264FramedSource::createNew(envir(), owner_);
        /* MPP 输出的是完整 NAL，必须用 Discrete 版；普通 Framer 会去扫起始码。 */
        return H264VideoStreamDiscreteFramer::createNew(envir(), src);
    }

    RTPSink* createNewRTPSink(Groupsock* rtpGroupsock,
                              unsigned char rtpPayloadTypeIfDynamic,
                              FramedSource* /*inputSource*/) override
    {
        /* 显式带上参数集，避免首个客户端的 SDP 缺 sprop-parameter-sets。 */
        std::vector<uint8_t> sps;
        std::vector<uint8_t> pps;
        if(owner_->copy_sps_pps(sps, pps) && !sps.empty() && !pps.empty()) {
            UPC_LOG_MEDIA_SERVICE(Info, "local_rtsp client SETUP sps=%zu pps=%zu",
                                  sps.size(), pps.size());
            return H264VideoRTPSink::createNew(
                envir(), rtpGroupsock, rtpPayloadTypeIfDynamic,
                sps.data(), static_cast<unsigned>(sps.size()),
                pps.data(), static_cast<unsigned>(pps.size()));
        }
        UPC_LOG_MEDIA_SERVICE(Warn,
            "local_rtsp SDP without sprop — parameter sets not seen yet");
        return H264VideoRTPSink::createNew(envir(), rtpGroupsock,
                                           rtpPayloadTypeIfDynamic);
    }

private:
    LocalRtspServer* owner_;
};

/** 大小写不敏感子串查找：RTSP 头字段与传输协议名都不区分大小写。 */
bool contains_ci(const char* hay, const char* needle)
{
    if(!hay || !needle) {
        return false;
    }
    const size_t nlen = std::strlen(needle);
    if(nlen == 0) {
        return true;
    }
    for(const char* p = hay; *p != '\0'; ++p) {
        if(strncasecmp(p, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * 判定 SETUP 是否请求 RTP-over-TCP（交织）。
 *
 * 请求 UDP 时 Transport 头形如 `RTP/AVP;unicast;client_port=5000-5001`，
 * 请求 TCP 时为 `RTP/AVP/TCP;unicast;interleaved=0-1`。
 * `RTP/AVP/TCP` 这个记号只会出现在 Transport 头里，故不必先切出该行。
 */
bool setup_wants_tcp(const char* full_request)
{
    return contains_ci(full_request, "RTP/AVP/TCP");
}

/**
 * 借派生类型转换取得 RTSPClientConnection 的 protected 成员访问权。
 *
 * `handleCmd_unsupportedTransport()` 在 live555 里只对 RTSPClientSession 开放
 * （靠友元声明），而友元关系不随继承传递——下面的 TcpOnlyClientSession 并不在
 * 友元名单内。这里只做指针类型转换、从不构造 ConnAccessor 对象；被调的是虚函数，
 * 最终仍分派到真实对象的实现。
 */
class ConnAccessor : public RTSPServer::RTSPClientConnection {
public:
    static void reject_transport(RTSPServer::RTSPClientConnection* conn)
    {
        static_cast<ConnAccessor*>(conn)->handleCmd_unsupportedTransport();
    }
};

/**
 * 只接受 RTP-over-TCP 的客户端会话。
 *
 * live555 只给了反向开关 `disableStreamingRTPOverTCP()`，没有"禁用 UDP"的接口，
 * 故在 SETUP 阶段自行判定，非 TCP 一律回 461 Unsupported Transport。
 *
 * 461 是 RTSP 标准的传输协商失败码：ffmpeg / VLC 收到后会自动改用 TCP 重发 SETUP，
 * 所以客户端即使没写 `-rtsp_transport tcp` 也能连上，只是多一轮握手。
 * 强制 TCP 的理由是 WiFi 下 UDP 丢包会直接导致花屏与 `missed packets`，
 * 而本地拉流都在局域网内，交织传输的开销可以忽略。
 */
class TcpOnlyClientSession : public RTSPServer::RTSPClientSession {
public:
    TcpOnlyClientSession(RTSPServer& server, u_int32_t session_id)
        : RTSPClientSession(server, session_id)
    {
    }

protected:
    void handleCmd_SETUP(RTSPServer::RTSPClientConnection* conn,
                         char const* url_pre_suffix,
                         char const* url_suffix,
                         char const* full_request) override
    {
        if(!setup_wants_tcp(full_request)) {
            UPC_LOG_MEDIA_SERVICE(Info,
                "local_rtsp SETUP over UDP rejected (461) — client should retry via TCP");
            ConnAccessor::reject_transport(conn);
            return;
        }
        RTSPClientSession::handleCmd_SETUP(conn, url_pre_suffix, url_suffix,
                                           full_request);
    }
};

/** 除了改用 TcpOnlyClientSession，其余行为与 RTSPServer 一致。 */
class TcpOnlyRtspServer : public RTSPServer {
public:
    static TcpOnlyRtspServer* createNew(UsageEnvironment& env, Port port)
    {
        /* setUpOurSocket 取 Port&（端口传 0 时会回填实际端口），故不能传临时量。 */
        int socket_v4 = setUpOurSocket(env, port, AF_INET);
        int socket_v6 = setUpOurSocket(env, port, AF_INET6);
        if(socket_v4 < 0 && socket_v6 < 0) {
            return nullptr;
        }
        return new TcpOnlyRtspServer(env, socket_v4, socket_v6, port);
    }

protected:
    /** authDatabase=nullptr：一期不做鉴权，局域网内可直接拉流。 */
    TcpOnlyRtspServer(UsageEnvironment& env, int socket_v4, int socket_v6, Port port)
        : RTSPServer(env, socket_v4, socket_v6, port, nullptr, kReclamationSeconds)
    {
    }

    ClientSession* createNewClientSession(u_int32_t session_id) override
    {
        return new TcpOnlyClientSession(*this, session_id);
    }

private:
    /** 与 RTSPServer::createNew 的默认值一致：超时无 RTSP/RTCP 交互则回收会话。 */
    static constexpr unsigned kReclamationSeconds = 65;
};

/** trigger 回调：有新帧待投递。 */
void frame_trigger_handler(void* client_data)
{
    static_cast<LocalRtspServer*>(client_data)->deliver_pending_frames();
}

/** trigger 回调：有新命令待执行。 */
void cmd_trigger_handler(void* client_data)
{
    static_cast<LocalRtspServer*>(client_data)->run_pending_cmds();
}

} // namespace

#endif /* UPC_MEDIA_HAS_LIVE555 */

LocalRtspServer::~LocalRtspServer()
{
    shutdown();
}

/** 绑定共用编码器；必须在 start() 之前注入。 */
void LocalRtspServer::set_encoder(H264StreamEncoder* encoder)
{
    std::lock_guard<std::mutex> lock(mtx_);
    encoder_ = encoder;
}

/** 当前开关状态。 */
bool LocalRtspServer::active() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return active_;
}

/** rtsp://<本机IP>:<port>/live */
std::string LocalRtspServer::url() const
{
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        port = port_;
    }
    std::string ip = local_ipv4();
    if(ip.empty()) {
        ip = "0.0.0.0";
    }
    return "rtsp://" + ip + ":" + std::to_string(port) + "/" +
           UPC_LOCAL_RTSP_STREAM_NAME;
}

/** 枚举可用于局域网拉流的 IPv4，按接口优先级排序。 */
std::vector<LocalRtspServer::LanAddr> LocalRtspServer::lan_ipv4_list()
{
    struct ifaddrs* ifa_list = nullptr;
    if(::getifaddrs(&ifa_list) != 0 || !ifa_list) {
        return {};
    }

    std::vector<LanAddr> result;
    for(struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if(!ifa->ifa_addr || !ifa->ifa_name || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if((ifa->ifa_flags & IFF_UP) == 0 || (ifa->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        if(is_virtual_iface(ifa->ifa_name)) {
            continue;
        }
        const auto* addr = reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_addr);
        if(is_link_local(addr)) {
            continue;
        }
        char buf[INET_ADDRSTRLEN]{};
        if(!::inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf))) {
            continue;
        }
        result.push_back(LanAddr{ifa->ifa_name, buf});
    }
    ::freeifaddrs(ifa_list);

    /* stable_sort：同优先级的接口保持内核给出的原始顺序 */
    std::stable_sort(result.begin(), result.end(),
                     [](const LanAddr& lhs, const LanAddr& rhs) {
                         return iface_rank(lhs.iface.c_str()) >
                                iface_rank(rhs.iface.c_str());
                     });
    return result;
}

/** 取首选局域网 IPv4；无可用地址返回空串。 */
std::string LocalRtspServer::local_ipv4()
{
    const auto list = lan_ipv4_list();
    return list.empty() ? std::string{} : list.front().ip;
}

/** 取一个待发送 NAL；队列空返回 false。 */
bool LocalRtspServer::pop_nal(NalUnit& out)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(nal_queue_.empty()) {
        return false;
    }
    out = std::move(nal_queue_.front());
    nal_queue_.pop_front();
    ++stat_nal_out_;
    return true;
}

/**
 * 记录当前活跃的 FramedSource（事件循环线程）。
 *
 * reuseFirstSource=True 下 source 只在**首个**客户端 SETUP 时创建，
 * 故这条日志等价于"开始有人拉流"；后续客户端复用同一 source，不再打点。
 */
void LocalRtspServer::attach_source(void* source)
{
    current_source_ = source;
    has_client_.store(true);
    UPC_LOG_MEDIA_SERVICE(Info, "local_rtsp client stream started");
}

/** source 析构时解绑（事件循环线程）：最后一个客户端离开或 session 被撤下。 */
void LocalRtspServer::detach_source(void* source)
{
    if(current_source_ == source) {
        current_source_ = nullptr;
        has_client_.store(false);
        UPC_LOG_MEDIA_SERVICE(Info, "local_rtsp client stream ended");
    }
}

/** 事件循环线程内取当前 source。 */
void* LocalRtspServer::current_source() const
{
    return current_source_;
}

/** 取编码器缓存的参数集。 */
bool LocalRtspServer::copy_sps_pps(std::vector<uint8_t>& sps,
                                   std::vector<uint8_t>& pps) const
{
    H264StreamEncoder* encoder = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        encoder = encoder_;
    }
    return encoder && encoder->copy_sps_pps(sps, pps);
}

/** 清空 NAL 队列。 */
void LocalRtspServer::clear_queue()
{
    std::lock_guard<std::mutex> lock(mtx_);
    nal_queue_.clear();
}

#if defined(UPC_MEDIA_HAS_LIVE555) && UPC_MEDIA_HAS_LIVE555

/**
 * 建立 RTSPServer 并启动常驻事件循环线程。
 * 事件触发器必须在事件循环启动前创建（createEventTrigger 非线程安全）。
 */
bool LocalRtspServer::init(uint16_t port)
{
    std::lock_guard<std::mutex> lock(mtx_);
    if(loop_running_) {
        return true;
    }
    port_ = port;

    /* 必须在任何 RTPSink 构造之前设好：OutPacketBuffer 在构造时就按 maxSize 定容，
     * 事后再改不影响已建好的 sink。默认 60000 装不下 1080×1280 的 I 帧。 */
    OutPacketBuffer::increaseMaxSizeTo(UPC_LOCAL_RTSP_MAX_FRAME_BYTES);

    auto* scheduler = BasicTaskScheduler::createNew();
    if(!scheduler) {
        UPC_LOG_MEDIA_SERVICE(Error, "local_rtsp scheduler create fail");
        return false;
    }
    UsageEnvironment* env = LogUsageEnvironment::createNew(*scheduler);
    if(!env) {
        UPC_LOG_MEDIA_SERVICE(Error, "local_rtsp env create fail");
        delete scheduler;
        return false;
    }
    /* 强制 RTP-over-TCP：UDP 的 SETUP 会被回 461，客户端自行改用 TCP 重试。 */
    RTSPServer* server = TcpOnlyRtspServer::createNew(*env, Port(port));
    if(!server) {
        UPC_LOG_MEDIA_SERVICE(Error, "local_rtsp listen :%u fail — %s",
                              static_cast<unsigned>(port), env->getResultMsg());
        env->reclaim();
        delete scheduler;
        return false;
    }

    frame_trigger_ = scheduler->createEventTrigger(frame_trigger_handler);
    cmd_trigger_ = scheduler->createEventTrigger(cmd_trigger_handler);

    scheduler_ = scheduler;
    env_ = env;
    rtsp_server_ = server;
    loop_stop_ = 0;
    loop_thread_ = std::thread(&LocalRtspServer::loop_fn, this);
    loop_running_ = true;

    UPC_LOG_MEDIA_SERVICE(Info, "local_rtsp server ready port=%u",
                          static_cast<unsigned>(port));
    return true;
}

/** 停事件循环并释放 live555 资源。 */
void LocalRtspServer::shutdown()
{
    stop();

    bool running = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        running = loop_running_;
    }
    if(running) {
        /* BasicTaskScheduler 每轮 SingleStep 都会检查 watchVariable，无需额外唤醒。 */
        loop_stop_ = 1;
        if(loop_thread_.joinable()) {
            loop_thread_.join();
        }
    }

    std::lock_guard<std::mutex> lock(mtx_);
    loop_running_ = false;
    if(rtsp_server_) {
        /* RTSPServer 析构会一并清理仍挂载的 ServerMediaSession。 */
        Medium::close(static_cast<RTSPServer*>(rtsp_server_));
        rtsp_server_ = nullptr;
        media_session_ = nullptr;
    }
    if(env_) {
        static_cast<UsageEnvironment*>(env_)->reclaim();
        env_ = nullptr;
    }
    if(scheduler_) {
        delete static_cast<BasicTaskScheduler*>(scheduler_);
        scheduler_ = nullptr;
    }
}

/** live555 事件循环线程体。 */
void LocalRtspServer::loop_fn()
{
    auto* env = static_cast<UsageEnvironment*>(env_);
    env->taskScheduler().doEventLoop(&loop_stop_);
    UPC_LOG_MEDIA_SERVICE(Info, "local_rtsp event loop exited");
}

/** 入队一条命令并触发事件循环处理。 */
void LocalRtspServer::post_cmd(Cmd cmd)
{
    UsageEnvironment* env = nullptr;
    uint32_t trigger = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!loop_running_) {
            return;
        }
        cmd_queue_.push_back(cmd);
        env = static_cast<UsageEnvironment*>(env_);
        trigger = cmd_trigger_;
    }
    if(env && trigger != 0) {
        env->taskScheduler().triggerEvent(trigger, this);
    }
}

/** 事件循环线程：执行命令队列中的全部命令。 */
void LocalRtspServer::run_pending_cmds()
{
    std::deque<Cmd> cmds;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cmds.swap(cmd_queue_);
    }

    auto* env = static_cast<UsageEnvironment*>(env_);
    auto* server = static_cast<RTSPServer*>(rtsp_server_);
    if(!env || !server) {
        return;
    }

    for(const Cmd cmd : cmds) {
        if(cmd == Cmd::AddSession) {
            if(media_session_) {
                continue;
            }
            ServerMediaSession* sms = ServerMediaSession::createNew(
                *env, UPC_LOCAL_RTSP_STREAM_NAME, UPC_LOCAL_RTSP_STREAM_NAME,
                "Snowpod camera live");
            if(!sms) {
                UPC_LOG_MEDIA_SERVICE(Error, "local_rtsp create sms fail");
                continue;
            }
            sms->addSubsession(LiveH264Subsession::createNew(*env, this));
            server->addServerMediaSession(sms);
            media_session_ = sms;
            UPC_LOG_MEDIA_SERVICE(Info, "local_rtsp session mounted");
        } else {
            if(!media_session_) {
                continue;
            }
            /* 断开在连客户端 + 撤下 session；仅 remove 的话已连客户端不会断。 */
            server->deleteServerMediaSession(
                static_cast<ServerMediaSession*>(media_session_));
            media_session_ = nullptr;
            UPC_LOG_MEDIA_SERVICE(Info, "local_rtsp session unmounted");
        }
    }
}

/** 唤醒事件循环去取新入队的 NAL。 */
void LocalRtspServer::notify_frame_ready()
{
    UsageEnvironment* env = nullptr;
    uint32_t trigger = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!loop_running_) {
            return;
        }
        env = static_cast<UsageEnvironment*>(env_);
        trigger = frame_trigger_;
    }
    if(env && trigger != 0) {
        env->taskScheduler().triggerEvent(trigger, this);
    }
}

/** 事件循环线程：把队列中的 NAL 交给当前 source。 */
void LocalRtspServer::deliver_pending_frames()
{
    void* src = current_source();
    if(!src) {
        return;
    }
    static_cast<H264FramedSource*>(src)->deliver_now();
}

#else /* 未编译 live555 支持：整体降级为空实现 */

bool LocalRtspServer::init(uint16_t port)
{
    port_ = port;
    UPC_LOG_MEDIA_SERVICE(Warn, "local_rtsp disabled in this build (no live555)");
    return false;
}

void LocalRtspServer::shutdown()
{
    stop();
}

void LocalRtspServer::loop_fn()
{
}

void LocalRtspServer::post_cmd(Cmd)
{
}

void LocalRtspServer::notify_frame_ready()
{
}

void LocalRtspServer::run_pending_cmds()
{
}

void LocalRtspServer::deliver_pending_frames()
{
}

#endif /* UPC_MEDIA_HAS_LIVE555 */

/**
 * 打开本地直播。
 *
 * 先注册编码器 sink（失败即返回，调用方据此把 UI 开关弹回），
 * 再把挂载 session 的动作投递给事件循环线程。
 * session 挂上之前到达的帧只是先入队，不影响正确性。
 */
bool LocalRtspServer::start()
{
    H264StreamEncoder* encoder = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!loop_running_) {
            UPC_LOG_MEDIA_SERVICE(Error, "local_rtsp start fail: server not ready");
            return false;
        }
        if(active_) {
            return true;
        }
        encoder = encoder_;
    }
    if(!encoder) {
        UPC_LOG_MEDIA_SERVICE(Error, "local_rtsp start fail: encoder not bound");
        return false;
    }

    /* 码率不指定：与云端推流共用编码器时沿用既有参数，独占时用板端默认。 */
    H264StreamEncoder::SinkOptions opt{};
    const int sink_id = encoder->add_sink(
        [this](const uint8_t* annexb, size_t len, bool, int64_t pts_us) {
            return on_encoded_packet(annexb, len, pts_us);
        },
        [this] { on_sink_dropped(); },
        opt);
    if(sink_id < 0) {
        UPC_LOG_MEDIA_SERVICE(Error, "local_rtsp start fail: encoder sink rejected");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mtx_);
        sink_id_ = sink_id;
        active_ = true;
        nal_queue_.clear();
        dropped_ = 0;
        stat_nal_in_ = 0;
        stat_nal_out_ = 0;
        stat_last_log_us_ = 0;
        stat_last_in_ = 0;
        stat_last_out_ = 0;
    }
    post_cmd(Cmd::AddSession);

    /* 多网口设备上首选地址未必就是客户端可达的那个（有线/无线各一张网卡很常见），
     * 把全部候选一并打出来，免得还要登设备查 `ip addr`。 */
    const auto addrs = lan_ipv4_list();
    std::string candidates;
    for(const auto& addr : addrs) {
        if(!candidates.empty()) {
            candidates += ' ';
        }
        candidates += addr.iface + "=" + addr.ip;
    }
    if(addrs.empty()) {
        UPC_LOG_MEDIA_SERVICE(Warn,
            "local_rtsp no usable LAN IPv4 — device not connected to a network?");
    }
    UPC_LOG_MEDIA_SERVICE(Info, "local_rtsp started url=%s candidates=[%s]",
                          url().c_str(), candidates.c_str());
    return true;
}

/**
 * 关闭本地直播。
 *
 * 先摘 sink（remove_sink 返回后不会再有帧入队），再撤 session。
 * 摘 sink 必须在不持 mtx_ 的情况下调用：编码器 worker 的分发回调
 * on_encoded_packet 需要 mtx_，持锁摘会两边互等。
 */
void LocalRtspServer::stop()
{
    H264StreamEncoder* encoder = nullptr;
    int sink_id = -1;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ && sink_id_ < 0) {
            return;
        }
        encoder = encoder_;
        sink_id = sink_id_;
        sink_id_ = -1;
        active_ = false;
    }
    if(encoder && sink_id >= 0) {
        encoder->remove_sink(sink_id);
    }
    post_cmd(Cmd::RemoveSession);
    clear_queue();
    UPC_LOG_MEDIA_SERVICE(Info, "local_rtsp stopped dropped=%u", dropped_);
}

/**
 * 编码器 sink 回调：拆 Annex-B 为裸 NAL 入队并唤醒事件循环。
 *
 * 队列满时丢最旧（实时优先）。始终返回 true：本地投递不会像管道那样断开，
 * 停流由 stop() 主动摘 sink 完成。
 *
 * pts_us 由编码器在采帧时统一打戳并透传，本帧全部 NAL 共用；不可在此处
 * 各自取时刻，否则同一 AU 的 NAL 会带不同 presentation time，live555 据此
 * 判定 AU 边界与 marker bit，客户端会报 "missing picture in access unit"。
 */
bool LocalRtspServer::on_encoded_packet(const uint8_t* annexb, size_t len,
                                        int64_t pts_us)
{
    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_) {
            return true;
        }
        H264StreamEncoder::for_each_nal(annexb, len,
            [this, pts_us, &queued](const uint8_t* nal, size_t nal_len) {
                if(nal_len == 0) {
                    return;
                }
                if(nal_queue_.size() >= UPC_LOCAL_RTSP_QUEUE_MAX) {
                    nal_queue_.pop_front();
                    ++dropped_;
                }
                NalUnit unit;
                unit.data.assign(nal, nal + nal_len);
                unit.pts_us = pts_us;
                nal_queue_.push_back(std::move(unit));
                ++stat_nal_in_;
                queued = true;
            });
        maybe_log_stats_locked();
    }
    if(queued) {
        notify_frame_ready();
    }
    return true;
}

/**
 * 每 5 秒打一行本地直播心跳。
 *
 * queue 持续贴近上限说明 live555 取不赢，端到端延迟会顶到队列容量对应的
 * 时长（默认 128 个 NAL、30fps 下约 4s）；in 涨而 out 不涨说明事件循环
 * 或客户端连接卡住了。调用前必须持有 mtx_。
 *
 * 由 UPC_MEDIA_LOG_STREAM_STAT 控制；关闭时本函数为空，计数照常累加。
 */
void LocalRtspServer::maybe_log_stats_locked()
{
#if UPC_MEDIA_LOG_STREAM_STAT
    const int64_t now = now_us();
    if(stat_last_log_us_ == 0) {
        stat_last_log_us_ = now;
        return;
    }
    const int64_t elapsed = now - stat_last_log_us_;
    if(elapsed < kStatsPeriodUs) {
        return;
    }

    const double secs = static_cast<double>(elapsed) / 1000000.0;
    const uint64_t d_in = stat_nal_in_ - stat_last_in_;
    const uint64_t d_out = stat_nal_out_ - stat_last_out_;
    UPC_LOG_MEDIA_SERVICE(Info,
        "local_rtsp stat in=%llu(%.1f/s) out=%llu(%.1f/s) queue=%zu/%u "
        "dropped=%u client=%d",
        static_cast<unsigned long long>(stat_nal_in_),
        static_cast<double>(d_in) / secs,
        static_cast<unsigned long long>(stat_nal_out_),
        static_cast<double>(d_out) / secs,
        nal_queue_.size(), UPC_LOCAL_RTSP_QUEUE_MAX,
        dropped_, has_client_.load() ? 1 : 0);

    stat_last_log_us_ = now;
    stat_last_in_ = stat_nal_in_;
    stat_last_out_ = stat_nal_out_;
#endif /* UPC_MEDIA_LOG_STREAM_STAT */
}

/** 编码器摘除本路 sink（编码器级故障）：撤下 session 并复位开关。 */
void LocalRtspServer::on_sink_dropped()
{
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if(!active_ && sink_id_ < 0) {
            return;
        }
        active_ = false;
        sink_id_ = -1;
    }
    UPC_LOG_MEDIA_SERVICE(Warn, "local_rtsp sink dropped — unmounting session");
    post_cmd(Cmd::RemoveSession);
    clear_queue();
}

} // namespace upc::media
