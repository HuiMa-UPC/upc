// Native integration test. Broker, VisualService, and a synthetic Camera each
// have their own process; the parent acts only as the Glavo IPC client.
// Pixel buffers use anonymous memfd files passed by production SCM_RIGHTS,
// while metadata uses the production SHM ring. No devices or motors are opened.
#include "visual_service.hpp"
#include "ipc_msg_bus.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace middleware;
using Clock = std::chrono::steady_clock;
constexpr uint32_t W = 320, H = 240, STRIDE = 352, SLOTS = 3;
#define CHECK(expr) do { if(!(expr)) throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #expr); } while(0)

struct Shared {
    std::atomic<bool> run{true};
    std::atomic<bool> tick_enabled{true}; // test-only scheduling seam; production code is unchanged
    std::atomic<int> mode{0}; // 0=no fresh frame; 1=black; 2=clear; 3=code; 4=plate
    std::atomic<unsigned> ready{0}, starts{0}, stops{0}, frames{0};
};

struct Harness {
    std::string directory, socket_path, ring_name;
    Shared* shared;
    std::vector<pid_t> children;
    Harness() {
        char temp[] = "/tmp/upc-cabin-ipc-XXXXXX";
        const char* path = ::mkdtemp(temp);
        CHECK(path);
        directory = path;
        socket_path = directory + "/broker.sock";
        ring_name = "/upc_cabin_ipc_" + std::to_string(::getpid());
        void* map = ::mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(map != MAP_FAILED);
        shared = new(map) Shared;
    }
    ~Harness() {
        shared->run = false;
        for(pid_t pid : children) {
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            int status = 0;
            while(::waitpid(pid, &status, WNOHANG) == 0) {
                if(Clock::now() > deadline) {
                    ::kill(pid, SIGKILL); ::waitpid(pid, &status, 0); break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        MsgPort::instance().shutdown();
        ::shm_unlink(ring_name.c_str());
        ::unlink(socket_path.c_str());
        ::unlink((directory + "/marker.png").c_str());
        ::unlink((directory + "/plate.json").c_str());
        ::unlink((directory + "/door.json").c_str());
        ::rmdir(directory.c_str());
        shared->~Shared(); ::munmap(shared, sizeof(Shared));
    }
    void spawn(const std::function<void()>& task) {
        const pid_t pid = ::fork();
        CHECK(pid >= 0);
        if(pid == 0) {
            try { task(); ::_exit(0); }
            catch(const std::exception& e) { std::cerr << "child: " << e.what() << '\n'; ::_exit(2); }
        }
        children.push_back(pid);
    }
};

template<class T> void send(MsgType type, const T& payload, ModuleId target = ModuleId::APP_VISUAL) {
    Message msg(type, ModuleId::APP_GLAVO, target);
    msg.payload = MsgPayload::from_bytes(reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    CHECK(MsgPort::instance().publish_async(msg));
}

void wait_until(const std::function<bool()>& predicate, int timeout_ms = 3000) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while(!predicate()) {
        CHECK(Clock::now() < deadline);
        MsgPort::instance().process(5);
    }
}
void pump_for(int milliseconds) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(milliseconds);
    while(Clock::now() < deadline) MsgPort::instance().process(5);
}

std::vector<uint8_t> to_nv12(const cv::Mat& gray) {
    cv::Mat bgr, planar;
    cv::cvtColor(gray,bgr,cv::COLOR_GRAY2BGR);
    cv::cvtColor(bgr,planar,cv::COLOR_BGR2YUV_I420);
    std::vector<uint8_t> out(W*H*3/2);
    std::memcpy(out.data(),planar.data,W*H);
    for(size_t i=0; i<W*H/4; ++i) {
        out[W*H+2*i] = planar.data[W*H+i];
        out[W*H+2*i+1] = planar.data[W*H*5/4+i];
    }
    return out;
}

std::vector<std::vector<uint8_t>> fixtures(const std::string& dir) {
    cv::RNG rng(0x6146);
    cv::Mat background(H,W,CV_8UC1);
    rng.fill(background,cv::RNG::UNIFORM,55,145);
    cv::Mat marker(64,64,CV_8UC1);
    for(int y=0;y<64;y+=8) for(int x=0;x<64;x+=8)
        marker(cv::Rect(x,y,8,8)).setTo(rng.uniform(0,2) ? 220 : 40);
    cv::Mat coded = background.clone();
    marker.copyTo(coded(cv::Rect(128,80,64,64)));
    CHECK(cv::imwrite(dir+"/marker.png",marker));
    std::ofstream config(dir+"/plate.json");
    config << R"({"version":1,"reference_size":[320,240],"search_roi":[0,0,1,1],
"search_margin":0.35,"template_gray_weight":0.7,
"high_confidence_threshold":0.78,"medium_confidence_threshold":0.58,
"hidden_max_confidence":0.42,"min_blur_variance":15,"min_dynamic_range":35,
"max_overexposed_fraction":0.35,"max_underexposed_fraction":0.35,"min_medium_markers":1,
"markers":[{"roi":[0.4,0.3333333333,0.6,0.6],"template":"marker.png"}]})";
    config.close();
    std::ofstream door_config(dir+"/door.json");
    door_config << R"({"max_mean_luma":24,"max_p95_luma":32,
"dark_pixel_threshold":40,"min_dark_fraction":0.98,
"clear_min_mean_luma":40,"clear_min_p95_luma":60})";
    door_config.close();
    std::vector<std::vector<uint8_t>> frames(5);
    frames[1] = to_nv12(cv::Mat(H,W,CV_8UC1,cv::Scalar(0)));
    frames[2] = to_nv12(cv::Mat(H,W,CV_8UC1,cv::Scalar(120)));
    frames[3] = to_nv12(coded);
    frames[4] = to_nv12(background);
    upc::visual::CabinDetector detector;
    std::string error;
    CHECK(detector.load_plate_config(dir+"/plate.json",error));
    CHECK(detector.plate(frames[3].data(),frames[3].size(),W,H,63).result==1);
    CHECK(detector.plate(frames[4].data(),frames[4].size(),W,H,63).result==2);
    return frames;
}

void camera_loop(Harness& h, const std::vector<std::vector<uint8_t>>& frames) {
    auto& port = MsgPort::instance();
    CHECK(port.set_mode_ipc_client(ModuleId::APP_CAMERA,h.socket_path.c_str()));
    upc_ipc::IpcDmaFrameRingProducer ring;
    CHECK(ring.create(h.ring_name.c_str(),W,H,STRIDE,upc_ipc::kDmaFrameFormatNv12,SLOTS)==upc_ipc::IpcShmError::OK);
    std::vector<int> fds;
    std::vector<uint8_t*> maps;
    const size_t bytes = STRIDE*H*3/2;
    for(unsigned i=0;i<SLOTS;++i) {
        int fd = ::memfd_create("cabin-test-frame",MFD_CLOEXEC);
        CHECK(fd>=0 && ::ftruncate(fd,static_cast<off_t>(bytes))==0);
        auto* memory = static_cast<uint8_t*>(::mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));
        CHECK(memory!=MAP_FAILED);
        fds.push_back(fd); maps.push_back(memory);
    }
    bool streaming = false;
    auto start = port.subscribe(MsgType::VISUAL_Camera_Stream_Start_Req,ModuleId::APP_CAMERA,
        [&](const Message& incoming) {
            CHECK(incoming.sender_id==ModuleId::APP_VISUAL);
            CameraStreamInfoPayload info{};
            std::snprintf(info.shm_name,sizeof(info.shm_name),"%s",h.ring_name.c_str());
            info.width=W; info.height=H; info.stride_y=STRIDE;
            info.format=upc_ipc::kDmaFrameFormatNv12; info.fps=25;
            info.dma_buf_count=SLOTS; info.ok=1; info.dma_mode=1;
            Message reply(MsgType::APP_Visual_Camera_Stream_Start_Rsp,ModuleId::APP_CAMERA,ModuleId::APP_VISUAL);
            reply.payload=MsgPayload::from_bytes(reinterpret_cast<const uint8_t*>(&info),sizeof(info));
            CHECK(port.publish_to_peer_with_fds(reply,fds));
            streaming=true; ++h.shared->starts;
        });
    auto stop = port.subscribe(MsgType::VISUAL_Camera_Stream_Stop_Req,ModuleId::APP_CAMERA,
        [&](const Message&) { streaming=false; ++h.shared->stops; });
    h.shared->ready.fetch_or(4);
    auto next_frame = Clock::now();
    while(h.shared->run) {
        port.process(5);
        const int mode=h.shared->mode;
        if(!streaming || mode==0 || Clock::now()<next_frame) continue;
        next_frame=Clock::now()+std::chrono::milliseconds(40);
        auto slot=ring.acquire_write_slot();
        if(!slot) continue;
        const auto& pixels=frames.at(static_cast<size_t>(mode));
        for(unsigned y=0;y<H*3/2;++y)
            std::memcpy(maps[*slot]+y*STRIDE,pixels.data()+y*W,W);
        const auto timestamp=std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        CHECK(ring.commit_slot(*slot,static_cast<uint64_t>(timestamp),*slot)>0);
        ++h.shared->frames;
    }
    start.unsubscribe(); stop.unsubscribe(); port.shutdown();
    for(unsigned i=0;i<SLOTS;++i) { ::munmap(maps[i],bytes); ::close(fds[i]); }
    ring.destroy();
}

int main() {
    try {
        cv::setNumThreads(1);
        Harness h;
        const auto frames=fixtures(h.directory);
        CHECK(::setenv("UPC_CABIN_PLATE_CONFIG",(h.directory+"/plate.json").c_str(),1)==0);
        CHECK(::setenv("UPC_CABIN_DOOR_CONFIG",(h.directory+"/door.json").c_str(),1)==0);
        h.spawn([&] {
            auto& bus=upc_ipc::IpcMsgBus::instance();
            CHECK(bus.init_broker(h.socket_path.c_str()));
            h.shared->ready.fetch_or(1);
            while(h.shared->run) bus.process(5);
            bus.shutdown();
        });
        // Before creating any parent MsgPort state, let the broker bind.
        const auto ready_deadline=Clock::now()+std::chrono::seconds(3);
        while(!(h.shared->ready.load()&1)) {
            CHECK(Clock::now()<ready_deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        h.spawn([&] {
            auto& port=MsgPort::instance();
            CHECK(port.set_mode_ipc_client(ModuleId::APP_VISUAL,h.socket_path.c_str()));
            upc::visual::VisualService service;
            CHECK(service.init()); h.shared->ready.fetch_or(2);
            while(h.shared->run) {
                port.process(5);
                if(h.shared->tick_enabled) service.tick();
            }
            service.shutdown(); port.shutdown();
        });
        h.spawn([&] { camera_loop(h,frames); });
        auto& port=MsgPort::instance();
        CHECK(port.set_mode_ipc_client(ModuleId::APP_GLAVO,h.socket_path.c_str()));
        std::vector<std::pair<bool,VisualCabinCheckRspPayload>> responses;
        auto collect=[&](const Message& msg, bool door) {
            CHECK(msg.sender_id==ModuleId::APP_VISUAL && msg.target_id==ModuleId::APP_GLAVO);
            CHECK(msg.payload.type==MsgPayload::Type::Inline && msg.payload.length==sizeof(VisualCabinCheckRspPayload));
            VisualCabinCheckRspPayload p{}; std::memcpy(&p,msg.payload.data.bytes.data(),sizeof(p));
            responses.emplace_back(door,p);
        };
        auto door_sub=port.subscribe(MsgType::APP_Visual_InnerDoor_Occlusion_Rsp,ModuleId::APP_GLAVO,
                                    [&](const Message& msg) { collect(msg,true); });
        auto plate_sub=port.subscribe(MsgType::APP_Visual_Plate_Present_Rsp,ModuleId::APP_GLAVO,
                                     [&](const Message& msg) { collect(msg,false); });
        wait_until([&] { return h.shared->ready==7 && port.peer_connected(ModuleId::APP_VISUAL) && port.peer_connected(ModuleId::APP_CAMERA); });
        send(MsgType::GLAVO_Visual_Cabin_Session_Start_Ntf,VisualCabinSessionPayload{1});
        wait_until([&] { return h.shared->starts>0; });
        auto request=[&](uint32_t id, bool door, uint32_t timeout=2000, uint8_t mask=63, uint8_t mode=0, uint32_t session=1) {
            send(door ? MsgType::GLAVO_Visual_InnerDoor_Occlusion_Req : MsgType::GLAVO_Visual_Plate_Present_Req,
                 VisualCabinCheckReqPayload{session,id,timeout,mask,mode,{0,0}});
        };
        auto response=[&](uint32_t id, bool door, int timeout=3000) {
            auto found=[&] { return std::find_if(responses.begin(),responses.end(),[&](const auto& item) { return item.first==door && item.second.request_id==id; }); };
            wait_until([&] { return found()!=responses.end(); },timeout);
            auto it=found(); const auto result=it->second; responses.erase(it); return result;
        };

        // A real request traverses the broker even without frames; duplicate
        // active requests do not restart its timeout or emit a second result.
        request(1,true,300); request(1,true,300);
        CHECK(response(1,true).result==0);
        pump_for(80); CHECK(responses.empty());
        request(1,true,300); CHECK(response(1,true).result==0); // cached terminal replay
        request(2,true,2000,0); CHECK(response(2,true).result==0); // lights off
        request(3,false,2000,63,1); CHECK(response(3,false).result==0); // invalid plate mode
        request(4,true,2000,63,0,99); CHECK(response(4,true).result==0); // wrong session
        send(MsgType::GLAVO_Visual_InnerDoor_Occlusion_Req,
             VisualCabinCheckReqPayload{1,4,2000,63,0,{1,0}});
        CHECK(response(4,true).result==0); // reserved bytes are not silently interpreted

        // Malformed requests are ignored, never executed.
        Message foreign(MsgType::GLAVO_Visual_InnerDoor_Occlusion_Req,
                        ModuleId::APP_GLAVO,ModuleId::APP_VISUAL);
        foreign.payload=MsgPayload::from_bytes(reinterpret_cast<const uint8_t*>("bad"),3);
        CHECK(port.publish_async(foreign));
        pump_for(80); CHECK(responses.empty());

        // WaitOpen ignores clear frames, then accepts stable full black through
        // NV12 -> SHM ring -> SCM_RIGHTS mmap -> production detector.
        h.shared->mode=2;
        request(5,true,3000,63,1);
        pump_for(550); CHECK(responses.empty());
        h.shared->mode=1;
        auto opened=response(5,true); CHECK(opened.result==1 && opened.frame_seq>0);
        request(5,true,3000,63,1); CHECK(response(5,true).result==1); // identical completed retry
        request(5,true,3000,63,0); CHECK(response(5,true).result==0); // cannot mutate the same ID
        h.shared->mode=2;
        request(6,true); CHECK(response(6,true).result==2);
        request(5,true); CHECK(response(5,true).result==0); // stale request cannot resurrect a result

        // Plate code requests stop on QrFound. Continued incoming plate frames
        // produce no unsolicited recheck; a NEW request is mandatory.
        h.shared->mode=3;
        request(7,false); auto code=response(7,false); CHECK(code.result==1 && code.frame_seq>0);
        wait_until([&] { return h.shared->stops>0; });
        const auto completed_starts=h.shared->starts.load();
        h.shared->mode=4;
        pump_for(1300); CHECK(responses.empty());
        CHECK(h.shared->starts==completed_starts); // no automatic stream restart or detection
        request(8,false,3000); auto present=response(8,false); CHECK(present.result==2 && present.frame_seq>code.frame_seq);

        // Repeated cached snapshots are not fresh frames and cannot pass.
        h.shared->mode=0; pump_for(100);
        request(9,false,500); CHECK(response(9,false).result==0);

        // Matching Stop cancels without late reply; replayed Start is not a new session.
        request(10,true,400); pump_for(40);
        send(MsgType::GLAVO_Visual_Cabin_Session_Stop_Ntf,VisualCabinSessionPayload{1});
        pump_for(550); CHECK(responses.empty());
        wait_until([&] { return h.shared->stops>0; });
        const auto starts=h.shared->starts.load();
        send(MsgType::GLAVO_Visual_Cabin_Session_Start_Ntf,VisualCabinSessionPayload{1});
        pump_for(120); CHECK(h.shared->starts==starts);
        request(11,true); CHECK(response(11,true).result==0);
        send(MsgType::GLAVO_Visual_Cabin_Session_Start_Ntf,VisualCabinSessionPayload{2});
        wait_until([&] { return h.shared->starts>starts; });
        h.shared->mode=1;
        request(1,true,2000,63,1,2);
        CHECK(response(1,true).result==1);

        // Old Stop cannot kill a newer session. Two tasks never run in parallel.
        send(MsgType::GLAVO_Visual_Cabin_Session_Stop_Ntf,VisualCabinSessionPayload{1});
        h.shared->mode=2;
        request(2,true,700,63,1,2);
        request(3,false,2000,63,0,2); CHECK(response(3,false).result==0);
        CHECK(response(2,true).result==0); // bounded WaitOpen times out while clear
        h.shared->mode=1;
        request(3,false,1000,63,0,2); CHECK(response(3,false).result==0); // black is not plate present

        // A legacy visual session has priority: cancel cabin with unknown, but
        // never stop the legacy stream when an old cabin Stop subsequently arrives.
        h.shared->mode=0;
        request(4,true,2000,63,1,2); pump_for(80);
        Message optical_start(MsgType::GLAVO_Visual_OpticalPath_Session_Start_Ntf,
                              ModuleId::APP_GLAVO,ModuleId::APP_VISUAL);
        CHECK(port.publish_async(optical_start));
        CHECK(response(4,true).result==0);
        pump_for(100);
        const auto optical_stops=h.shared->stops.load();
        send(MsgType::GLAVO_Visual_Cabin_Session_Stop_Ntf,VisualCabinSessionPayload{2});
        pump_for(100); CHECK(responses.empty());
        CHECK(h.shared->stops==optical_stops);
        send(MsgType::GLAVO_Visual_Cabin_Session_Start_Ntf,VisualCabinSessionPayload{3});
        request(1,true,300,63,1,3); CHECK(response(1,true).result==0); // busy with legacy
        Message optical_stop(MsgType::GLAVO_Visual_OpticalPath_Session_Stop_Ntf,
                             ModuleId::APP_GLAVO,ModuleId::APP_VISUAL);
        CHECK(port.publish_async(optical_stop));
        wait_until([&] { return h.shared->stops>optical_stops; });

        // Keep receiving IPC and frames but withhold periodic result drainage.
        // Stop must discard any positive result queued by the detector worker.
        send(MsgType::GLAVO_Visual_Cabin_Session_Start_Ntf,VisualCabinSessionPayload{3});
        h.shared->mode=1;
        h.shared->tick_enabled=false;
        request(1,true,2000,63,1,3);
        pump_for(1000); CHECK(responses.empty());
        send(MsgType::GLAVO_Visual_Cabin_Session_Stop_Ntf,VisualCabinSessionPayload{3});
        pump_for(100);
        h.shared->tick_enabled=true;
        pump_for(100); CHECK(responses.empty());

        // A lost caller after Start cannot keep the camera stream forever.
        const auto lease_starts=h.shared->starts.load(), lease_stops=h.shared->stops.load();
        send(MsgType::GLAVO_Visual_Cabin_Session_Start_Ntf,VisualCabinSessionPayload{4});
        h.shared->mode=0;
        wait_until([&] { return h.shared->starts>lease_starts; });
        wait_until([&] { return h.shared->stops>lease_stops; },16500);
        CHECK(responses.empty());
        request(1,true,300,63,1,4); CHECK(response(1,true).result==0);

        // Missing on-disk calibration is a terminal unknown, not an implicit
        // default classifier. A new request does not reuse the prior calibration.
        CHECK(::unlink((h.directory+"/door.json").c_str())==0);
        send(MsgType::GLAVO_Visual_Cabin_Session_Start_Ntf,VisualCabinSessionPayload{5});
        h.shared->mode=1;
        request(1,true,2000,63,1,5); CHECK(response(1,true).result==0);
        CHECK(::unlink((h.directory+"/plate.json").c_str())==0);
        h.shared->mode=4;
        request(2,false,2000,63,0,5); CHECK(response(2,false).result==0);
        send(MsgType::GLAVO_Visual_Cabin_Session_Stop_Ntf,VisualCabinSessionPayload{5});
        pump_for(100); CHECK(responses.empty());
        std::cout << "Native broker/Visual/camera IPC tests passed: full NV12 door and plate, stop/recheck, IDs, timeout, cancel\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
