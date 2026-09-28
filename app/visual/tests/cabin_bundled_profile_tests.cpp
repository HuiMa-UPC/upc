#include "cabin_detector.hpp"
#include "cabin_bundled_profile.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace upc::visual;
#define CHECK(expression) do { if(!(expression)) { std::cerr << __LINE__ << ": " #expression "\n"; std::exit(1); } } while(0)

int main() {
    cv::setNumThreads(1);
    CabinDetector detector;
    std::string error;
    CHECK(std::string(cabin_bundled_profile_id()) == "station_172_17_101_78_zminus20_candidate_v1");
    CHECK(std::string(cabin_bundled_profile_sha256()).size() == 64);
    CHECK(detector.load_bundled_door_config(error));
    CHECK(error.empty());
    CHECK(detector.load_bundled_plate_config(error));
    CHECK(error.empty());

    // Reconstruct a synthetic positive frame using only the bytes compiled into
    // this executable. This test reads no runtime calibration/template files.
    const auto& json = bundled_profile::kPlateJson;
    cv::FileStorage fs(std::string(reinterpret_cast<const char*>(json), sizeof(json)),
                       cv::FileStorage::READ | cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
    CHECK(fs.isOpened());
    const int width = static_cast<int>(fs["reference_size"][0]);
    const int height = static_cast<int>(fs["reference_size"][1]);
    cv::Mat gray(height, width, CV_8UC1);
    cv::RNG rng(12345);
    rng.fill(gray, cv::RNG::UNIFORM, 40, 140);
    cv::Mat one_glyph;
    for(const auto& marker : fs["markers"]) {
        const std::string name = static_cast<std::string>(marker["template"]);
        cv::Mat decoded;
        for(const auto& asset : bundled_profile::kTemplates) {
            if(name == asset.name) {
                cv::Mat encoded(1, static_cast<int>(asset.size), CV_8UC1,
                                const_cast<unsigned char*>(asset.data));
                decoded = cv::imdecode(encoded, cv::IMREAD_GRAYSCALE);
                break;
            }
        }
        CHECK(!decoded.empty());
        if(one_glyph.empty()) one_glyph = decoded.clone();
        const auto roi = marker["roi"];
        const int left = static_cast<int>(std::nearbyint(static_cast<double>(roi[0]) * width));
        const int top = static_cast<int>(std::nearbyint(static_cast<double>(roi[1]) * height));
        const int right = static_cast<int>(std::nearbyint(static_cast<double>(roi[2]) * width));
        const int bottom = static_cast<int>(std::nearbyint(static_cast<double>(roi[3]) * height));
        cv::Mat resized;
        cv::resize(decoded, resized, cv::Size(right-left, bottom-top), 0, 0, cv::INTER_AREA);
        resized.copyTo(gray(cv::Rect(left, top, right-left, bottom-top)));
    }
    cv::Mat bgr, i420;
    cv::cvtColor(gray, bgr, cv::COLOR_GRAY2BGR);
    cv::cvtColor(bgr, i420, cv::COLOR_BGR2YUV_I420);
    std::vector<uint8_t> nv12(i420.data, i420.data + gray.total()*3/2);
    for(size_t n = 0; n < gray.total()/4; ++n) {
        nv12[gray.total()+2*n] = i420.data[gray.total()+n];
        nv12[gray.total()+2*n+1] = i420.data[gray.total()+gray.total()/4+n];
    }
    const auto result = detector.plate(nv12.data(), nv12.size(), width, height, 0x3f);
    if(result.result != 1) std::cerr << result.reason << " score=" << result.score << "\n";
    CHECK(result.result == 1);
    const auto as_nv12 = [](const cv::Mat& mono) {
        cv::Mat color, planar;
        cv::cvtColor(mono,color,cv::COLOR_GRAY2BGR);
        cv::cvtColor(color,planar,cv::COLOR_BGR2YUV_I420);
        std::vector<uint8_t> bytes(planar.data,planar.data+mono.total()*3/2);
        for(size_t n=0;n<mono.total()/4;++n) {
            bytes[mono.total()+2*n]=planar.data[mono.total()+n];
            bytes[mono.total()+2*n+1]=planar.data[mono.total()+mono.total()/4+n];
        }
        return bytes;
    };
    // Seeing just one etched glyph away from all five calibrated local windows
    // is sufficient; neither its payload nor exact capture Z is consulted.
    // A blurry code-like decoy elsewhere must not count by itself or hide it.
    cv::Mat decoy_scene(height,width,CV_8UC1), moved, smaller, blurred;
    rng.fill(decoy_scene,cv::RNG::UNIFORM,40,140);
    cv::GaussianBlur(one_glyph,blurred,cv::Size(11,11),3.0);
    cv::resize(blurred,blurred,cv::Size(38,34),0,0,cv::INTER_AREA);
    blurred.copyTo(decoy_scene(cv::Rect(370,540,blurred.cols,blurred.rows)));
    const auto decoy_nv12=as_nv12(decoy_scene);
    const auto no_code_result=detector.plate(decoy_nv12.data(),decoy_nv12.size(),width,height,0x3f);
    if(no_code_result.result != 2)
        std::cerr << "synthetic no-code plate: " << no_code_result.reason
                  << " score=" << no_code_result.score << "\n";
    CHECK(no_code_result.result == 2);
    moved = decoy_scene.clone();
    cv::resize(one_glyph,smaller,cv::Size(static_cast<int>(std::nearbyint(one_glyph.cols*.70)),
                                          static_cast<int>(std::nearbyint(one_glyph.rows*.70))),
               0,0,cv::INTER_AREA);
    smaller.copyTo(moved(cv::Rect(560,780,smaller.cols,smaller.rows)));
    const auto moved_nv12=as_nv12(moved);
    const auto moved_result=detector.plate(moved_nv12.data(),moved_nv12.size(),width,height,0x3f);
    if(moved_result.result != 1) std::cerr << moved_result.reason << " score=" << moved_result.score << "\n";
    CHECK(moved_result.result == 1);
    CHECK(moved_result.reason == "code_visible_wide_search");

    // One clearly visible glyph must still mean no plate when its apparent
    // size and position change with Z. None of these poses uses a calibrated
    // local marker window; the 0.45x and 2.0x cases extend beyond the old
    // 0.60x--1.60x coarse-search range. Keep the blurred decoy in each scene.
    struct GlyphPose { double scale; cv::Point top_left; };
    const std::vector<GlyphPose> poses{{.45,{380,450}}, {1.0,{750,800}},
                                       {2.0,{570,410}}};
    const auto search_roi=fs["search_roi"];
    const cv::Rect base(static_cast<int>(std::nearbyint(static_cast<double>(search_roi[0])*width)),
                        static_cast<int>(std::nearbyint(static_cast<double>(search_roi[1])*height)),
                        static_cast<int>(std::nearbyint(static_cast<double>(search_roi[2])*width))-
                            static_cast<int>(std::nearbyint(static_cast<double>(search_roi[0])*width)),
                        static_cast<int>(std::nearbyint(static_cast<double>(search_roi[3])*height))-
                            static_cast<int>(std::nearbyint(static_cast<double>(search_roi[1])*height)));
    for(const GlyphPose& pose : poses) {
        cv::Mat visible=decoy_scene.clone(), glyph;
        cv::resize(one_glyph,glyph,
                   cv::Size(static_cast<int>(std::nearbyint(one_glyph.cols*pose.scale)),
                            static_cast<int>(std::nearbyint(one_glyph.rows*pose.scale))),
                   0,0,pose.scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
        const cv::Rect location(pose.top_left,glyph.size());
        CHECK((location & base) == location);
        glyph.copyTo(visible(location));
        const auto bytes=as_nv12(visible);
        const auto found=detector.plate(bytes.data(),bytes.size(),width,height,0x3f);
        if(found.result != 1)
            std::cerr << "shifted glyph scale=" << pose.scale << " at " << pose.top_left
                      << ": " << found.reason << " score=" << found.score << "\n";
        CHECK(found.result == 1);
        CHECK(found.reason == "code_visible_wide_search");
    }
    CHECK(detector.plate(nv12.data(), nv12.size(), width, height, 0).result == 0);
    CHECK(detector.plate(nullptr, nv12.size(), width, height, 0x3f).result == 0);
    CHECK(detector.plate(nv12.data(), nv12.size(), height, width, 0x3f).reason == "aspect_changed");
    std::vector<uint8_t> black(nv12.size(), 128);
    std::fill(black.begin(), black.begin()+gray.total(), 16);
    CHECK(detector.door(black.data(), black.size(), width, height, 0x3f).result == 1);
    CHECK(detector.door(black.data(), black.size(), width, height, 0).result == 0);
    CHECK(detector.plate(black.data(), black.size(), width, height, 0x3f).result == 0);

    // Explicit failed file reloads do not silently retain or recover defaults.
    CHECK(!detector.load_plate_config("/nonexistent/upc-cabin-explicit-plate.json", error));
    CHECK(detector.plate(nv12.data(), nv12.size(), width, height, 0x3f).reason == "plate_calibration_unavailable");
    CHECK(!detector.load_door_config("/nonexistent/upc-cabin-explicit-door.json", error));
    CHECK(detector.door(black.data(), black.size(), width, height, 0x3f).result == 0);
    CHECK(detector.load_bundled_plate_config(error));
    CHECK(detector.load_bundled_door_config(error));
    CHECK(detector.plate(nv12.data(), nv12.size(), width, height, 0x3f).result == 1);
    std::cout << "bundled cabin profile tests passed: " << cabin_bundled_profile_id()
              << " sha256=" << cabin_bundled_profile_sha256() << "\n";
}
