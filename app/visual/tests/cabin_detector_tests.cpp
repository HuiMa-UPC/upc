#include "cabin_detector.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace upc::visual;
#define CHECK(expression) do { if(!(expression)) { std::cerr << __LINE__ << ": " #expression "\n"; std::exit(1); } } while(0)
int main() {
    cv::setNumThreads(1);
    CabinDetector detector;
    std::string error;
    const auto directory=std::filesystem::temp_directory_path() /
        ("upc-cabin-unit-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto write_door = [&](bool complete, bool bad_type = false) {
        cv::FileStorage fs((directory/"door.json").string(),cv::FileStorage::WRITE|cv::FileStorage::FORMAT_JSON);
        if(bad_type) fs << "max_mean_luma" << "dark";
        else fs << "max_mean_luma" << 24;
        fs << "max_p95_luma" << 32 << "dark_pixel_threshold" << 40;
        fs << "min_dark_fraction" << .98 << "clear_min_mean_luma" << 40;
        if(complete) fs << "clear_min_p95_luma" << 60;
    };
    std::vector<uint8_t> black(320*240*3/2,128);
    std::fill(black.begin(),black.begin()+320*240,16);
    CHECK(detector.door(black.data(),black.size(),320,240,0x3f).result == 0);
    write_door(true);
    CHECK(detector.load_door_config((directory/"door.json").string(),error));
    CHECK(detector.door(black.data(),black.size(),320,240,0x3f).result == 1);
    CHECK(detector.door(black.data(),black.size(),320,240,0).result == 0);
    CHECK(detector.door(black.data(),black.size(),320,240,0xff).result == 0);
    CHECK(detector.door(nullptr,black.size(),320,240,0x3f).result == 0);
    CHECK(detector.door(black.data(),black.size()-1,320,240,0x3f).result == 0);
    CHECK(detector.door(black.data(),black.size(),321,240,0x3f).result == 0);
    CHECK(detector.door(black.data(),black.size(),0xffffffff,0xffffffff,0x3f).result == 0);
    CHECK(detector.door(black.data(),8192u*8192u*3u/2u,8192,8192,0x3f).result == 0);
    CHECK(detector.plate(black.data(),black.size(),320,240,0x3f).result == 0);
    // Partially blocked image with 20% bright chamber cannot become full-open.
    std::fill(black.begin(),black.begin()+320*48,100);
    CHECK(detector.door(black.data(),black.size(),320,240,0x3f).result != 1);
    std::fill(black.begin(),black.begin()+320*240,120);
    CHECK(detector.door(black.data(),black.size(),320,240,0x3f).result == 2);
    std::fill(black.begin(),black.begin()+320*240,255);
    CHECK(detector.door(black.data(),black.size(),320,240,0x3f).result == 0);
    write_door(false);
    CHECK(!detector.load_door_config((directory/"door.json").string(),error));
    std::fill(black.begin(),black.begin()+320*240,16);
    CHECK(detector.door(black.data(),black.size(),320,240,0x3f).result == 0);
    write_door(true,true);
    CHECK(!detector.load_door_config((directory/"door.json").string(),error));
    write_door(true);
    CHECK(detector.load_door_config((directory/"door.json").string(),error));
    CHECK(!detector.load_plate_config("/nonexistent/cabin_calibration.json",error));
    CHECK(!error.empty());
    // A real loaded calibration must still reject a black/invalid scene.
    cv::Mat marker(40,40,CV_8UC1);
    cv::RNG rng(42); rng.fill(marker,cv::RNG::UNIFORM,70,240);
    CHECK(cv::imwrite((directory/"marker.png").string(),marker));
    const auto write_plate = [&](int invalid_case = 0, bool small_roi = false) {
        cv::FileStorage fs((directory/"plate.json").string(),cv::FileStorage::WRITE|cv::FileStorage::FORMAT_JSON);
        fs << "version" << (invalid_case == 1 ? 1.5 : 1.0);
        fs << "reference_size" << "[" << (invalid_case == 8 ? 8192 : 320) << (invalid_case == 8 ? 8192 : 240) << "]";
        fs << "search_roi" << "[" << .1 << .1 << .9 << .9 << "]";
        fs << "markers" << "[" << "{" << "roi" << "[" << (invalid_case == 3 ? .05 : .375) << (80./240.)
           << (small_roi ? .425 : .5) << (small_roi ? .425 : .5) << "]";
        fs << "template" << "marker.png" << "}" << "]";
        fs << "search_margin" << .35 << "template_gray_weight" << .7;
        fs << "high_confidence_threshold" << .78 << "medium_confidence_threshold" << .58;
        fs << "hidden_max_confidence" << (invalid_case == 4 ? .8 : .42);
        fs << "min_medium_markers" << (invalid_case == 2 ? 1.5 : (invalid_case == 5 ? 2.0 : 1.0));
        fs << "min_blur_variance" << 15.0 << "min_dynamic_range" << 35.0;
        if(invalid_case != 6) fs << "max_overexposed_fraction" << .35;
        if(invalid_case == 7) fs << "max_underexposed_fraction" << "invalid";
        else fs << "max_underexposed_fraction" << .35;
    };
    write_plate();
    CHECK(detector.load_plate_config((directory/"plate.json").string(),error));
    std::fill(black.begin(),black.begin()+320*240,16);
    CHECK(detector.plate(black.data(),black.size(),320,240,0x3f).result == 0);
    CHECK(detector.plate(black.data(),black.size(),320,240,0x01).result == 0);
    CHECK(detector.plate(nullptr,black.size(),320,240,0x3f).result == 0);
    CHECK(detector.plate(black.data(),black.size()+1,320,240,0x3f).result == 0);
    CHECK(detector.plate(black.data(),black.size(),0xffffffff,0xffffffff,0x3f).result == 0);
    cv::Mat scene(240,320,CV_8UC1);
    rng.fill(scene,cv::RNG::UNIFORM,50,170);
    const cv::Mat empty_scene = scene.clone();
    marker.copyTo(scene(cv::Rect(120,80,40,40)));
    cv::Mat bgr, i420; cv::cvtColor(scene,bgr,cv::COLOR_GRAY2BGR);
    cv::cvtColor(bgr,i420,cv::COLOR_BGR2YUV_I420);
    std::vector<uint8_t> nv12(i420.data,i420.data+scene.total()*3/2);
    for(size_t j=0;j<scene.total()/4;++j) {
        nv12[scene.total()+2*j]=i420.data[scene.total()+j];
        nv12[scene.total()+2*j+1]=i420.data[scene.total()+scene.total()/4+j];
    }
    CHECK(detector.plate(nv12.data(),nv12.size(),320,240,0x3f).result == 1);
    const auto as_nv12 = [](const cv::Mat& mono) {
        cv::Mat color, planar;
        cv::cvtColor(mono,color,cv::COLOR_GRAY2BGR);
        cv::cvtColor(color,planar,cv::COLOR_BGR2YUV_I420);
        std::vector<uint8_t> bytes(planar.data,planar.data+mono.total()*3/2);
        for(size_t j=0;j<mono.total()/4;++j) {
            bytes[mono.total()+2*j]=planar.data[mono.total()+j];
            bytes[mono.total()+2*j+1]=planar.data[mono.total()+mono.total()/4+j];
        }
        return bytes;
    };
    // Wider search must not treat a plain bright square as the etched glyph.
    cv::Mat blank = empty_scene.clone();
    cv::rectangle(blank,cv::Rect(190,145,30,30),cv::Scalar(230),cv::FILLED);
    const auto blank_nv12 = as_nv12(blank);
    CHECK(detector.plate(blank_nv12.data(),blank_nv12.size(),320,240,0x3f).result != 1);
    const auto empty_nv12 = as_nv12(empty_scene);
    CHECK(detector.plate(empty_nv12.data(),empty_nv12.size(),320,240,0x3f).result != 1);
    // A valid but tiny external marker profile cannot be searched reliably at
    // half resolution. When no local code is seen, return Unknown, not plate.
    write_plate(0,true);
    CHECK(detector.load_plate_config((directory/"plate.json").string(),error));
    const auto tiny_result=detector.plate(empty_nv12.data(),empty_nv12.size(),320,240,0x3f);
    CHECK(tiny_result.result == 0);
    CHECK(tiny_result.reason == "marker_too_small_for_wide_search");
    write_plate();
    CHECK(detector.load_plate_config((directory/"plate.json").string(),error));
    CHECK(detector.plate(nv12.data(),nv12.size(),240,320,0x3f).result == 0);
    for(int invalid_case = 1; invalid_case <= 8; ++invalid_case) {
        write_plate(invalid_case);
        CHECK(!detector.load_plate_config((directory/"plate.json").string(),error));
        CHECK(!error.empty());
        CHECK(detector.plate(nv12.data(),nv12.size(),320,240,0x3f).result == 0);
    }
    write_plate();
    CHECK(detector.load_plate_config((directory/"plate.json").string(),error));
    // Missing reloaded calibration cannot silently retain the previous usable templates.
    CHECK(!detector.load_plate_config((directory/"missing.json").string(),error));
    CHECK(detector.plate(nv12.data(),nv12.size(),320,240,0x3f).result == 0);
    std::filesystem::remove_all(directory);
    CabinEvidence wait_open(true,true,10,0,0);
    for(uint32_t n=11;n<30;++n) CHECK(wait_open.observe(2,n,n*50) == 0);
    for(uint32_t n=30;n<34;++n) CHECK(wait_open.observe(1,n,n*50) == 0);
    CHECK(wait_open.observe(1,34,1700) == 1);
    CabinEvidence snapshot(true,false,0,0,0);
    for(uint32_t n=1;n<5;++n) CHECK(snapshot.observe(2,n,n*50) == 0);
    CHECK(snapshot.observe(2,5,250) == 2);
    CabinEvidence plate(false,false,100,0,0);
    for(uint32_t n=0;n<30;++n) CHECK(plate.observe(2,100,100+n*100) == 0);
    CHECK(plate.observe(2,99,4000) == 0);
    for(uint32_t n=101;n<111;++n) CHECK(plate.observe(2,n,4100+(n-101)*100) == 0);
    CHECK(plate.observe(0,111,5100) == 0); // uncertainty breaks previous no-code streak
    for(uint32_t n=112;n<123;++n) CHECK(plate.observe(2,n,5200+(n-112)*100) == 0);
    CHECK(plate.observe(2,123,6300) == 2);
    CabinEvidence code(false,false,0,0,0);
    CHECK(code.observe(1,1,0) == 1); // one confidently visible code blocks printing
    CHECK(code.observe(1,1,25) == 0); // duplicate is not evidence
    CHECK(code.observe(1,2,50) == 1);
    CHECK(code.observe(1,3,100) == 1);
    CabinEvidence warmup(true,false,0,0,200);
    for(uint32_t n=1;n<=5;++n) CHECK(warmup.observe(1,n,n*30) == 0);
    // Frames observed during warmup must not contribute after it.
    CHECK(warmup.observe(1,6,200) == 0);
    CHECK(warmup.observe(1,7,240) == 0);
    CHECK(warmup.observe(1,8,280) == 0);
    CHECK(warmup.observe(1,9,320) == 0);
    CHECK(warmup.observe(1,10,360) == 1);
    CabinEvidence sequence_wrap(false,false,0xfffffffdu,0,0);
    CHECK(sequence_wrap.observe(1,0xfffffffeu,0) == 1);
    CHECK(sequence_wrap.observe(1,0xffffffffu,50) == 1);
    CHECK(sequence_wrap.observe(1,0,100) == 0); // reserved invalid frame sequence
    CHECK(sequence_wrap.observe(1,1,100) == 1);
    CabinEvidence stale(false,false,10,0,0);
    CHECK(stale.observe(1,11,0) == 1);
    CHECK(stale.observe(1,12,50) == 1);
    CHECK(stale.observe(1,11,100) == 0);
    CHECK(stale.observe(1,12,100) == 0);
    CHECK(stale.observe(1,13,100) == 1);
    CabinEvidence gap(true,false,0,0,0);
    CHECK(gap.observe(1,1,0) == 0);
    CHECK(gap.observe(1,2,50) == 0);
    CHECK(gap.observe(1,3,600) == 0);
    CHECK(gap.observe(1,4,650) == 0);
    CHECK(gap.observe(1,5,700) == 0);
    CHECK(gap.observe(1,6,750) == 0);
    CHECK(gap.observe(1,7,800) == 1);
    CabinEvidence backward(true,false,0,100,0);
    CHECK(backward.observe(1,1,100) == 0);
    CHECK(backward.observe(1,2,150) == 0);
    CHECK(backward.observe(1,3,120) == 0);
    CHECK(backward.observe(1,4,170) == 0);
    CHECK(backward.observe(1,5,220) == 0);
    CHECK(backward.observe(1,6,270) == 0);
    CHECK(backward.observe(1,7,320) == 1);
    CabinEvidence overflow(false,false,0,UINT64_MAX-100,200);
    CHECK(overflow.observe(1,1,UINT64_MAX-50) == 0);
    CHECK(overflow.observe(1,2,UINT64_MAX-25) == 0);
    CHECK(overflow.observe(1,3,UINT64_MAX) == 0);
    CHECK(!cabin_session_start_allowed(100,true,0,99,90000));
    CHECK(!cabin_session_start_allowed(100,false,1000,100,60000));
    CHECK(!cabin_session_start_allowed(100,false,1000,99,60000));
    CHECK(cabin_session_start_allowed(100,false,1000,99,61000));
    CHECK(cabin_session_start_allowed(100,false,1000,100,61000));
    CHECK(cabin_session_start_allowed(100,false,1000,0x90000000,9000000000ULL));
    CHECK(!cabin_session_start_allowed(100,true,1000,0x90000000,9000000000ULL));
    CHECK(cabin_session_start_allowed(0xfffffff0,true,0,10,1000));
    CHECK(!cabin_session_start_allowed(10,false,0,0,90000));
    CabinPlateSceneHold scene_hold;
    CHECK(!scene_hold.capture(nullptr,black.size(),320,240));
    CHECK(scene_hold.capture(black.data(),black.size(),320,240));
    CHECK(scene_hold.stable(black.data(),black.size(),320,240));
    auto changed = black;
    changed[100*320+100] = 200;
    CHECK(scene_hold.stable(changed.data(),changed.size(),320,240));
    for(int y=80;y<120;++y) for(int x=100;x<140;++x) changed[y*320+x] = 200;
    CHECK(!scene_hold.stable(changed.data(),changed.size(),320,240));
    CHECK(!scene_hold.stable(black.data(),black.size()-1,320,240));
    std::cout << "cabin detector/evidence tests passed\n";
}
