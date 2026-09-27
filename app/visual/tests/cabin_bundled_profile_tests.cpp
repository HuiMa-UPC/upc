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
