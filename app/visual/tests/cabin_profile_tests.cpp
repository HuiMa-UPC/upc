#include "cabin_detector.hpp"
#include "cabin_profile.hpp"
#include <opencv2/core.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace upc::visual;
#define CHECK(x) do { if(!(x)) throw std::runtime_error(std::string("line ")+std::to_string(__LINE__)+": " #x); } while(0)

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        ("upc-cabin-profile-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    try {
        CabinDetector detector;
        std::string source,error;
        const auto door = (directory/"door.json").string();
        const auto plate = (directory/"plate.json").string();
        const auto missing = (directory/"missing.json").string();
        std::vector<uint8_t> black(320*240*3/2,128);
        std::fill(black.begin(),black.begin()+320*240,16);
        // A fresh installation needs no operator-created JSON or template files.
        CHECK(load_cabin_profile(detector,true,nullptr,door,source,error));
        CHECK(source == std::string("bundled:")+cabin_bundled_profile_id());
        CHECK(error.empty());
        CHECK(detector.door(black.data(),black.size(),320,240,63).result==1);
        CHECK(load_cabin_profile(detector,false,nullptr,plate,source,error));
        CHECK(source.rfind("bundled:",0)==0);
        CHECK(std::filesystem::is_empty(directory)); // no extraction or runtime writes
        CHECK(detector.plate(black.data(),black.size(),320,240,63).result==0);

        // An explicit missing override is an error, not permission to guess.
        CHECK(!load_cabin_profile(detector,true,missing.c_str(),door,source,error));
        CHECK(source=="override:"+missing && !error.empty());
        CHECK(detector.door(black.data(),black.size(),320,240,63).result==0);
        CHECK(!load_cabin_profile(detector,false,missing.c_str(),plate,source,error));
        CHECK(source=="override:"+missing && !error.empty());

        // Existing station calibration takes priority. Stricter thresholds here
        // reject Y=16, demonstrating that the built-in config did not replace it.
        {
            std::ofstream out(door);
            out << R"({"max_mean_luma":5,"max_p95_luma":8,"dark_pixel_threshold":10,
"min_dark_fraction":0.98,"clear_min_mean_luma":40,"clear_min_p95_luma":60})";
        }
        CHECK(load_cabin_profile(detector,true,nullptr,door,source,error));
        CHECK(source=="station:"+door);
        CHECK(detector.door(black.data(),black.size(),320,240,63).result==0);
        CHECK(load_cabin_profile(detector,true,door.c_str(),missing,source,error));
        CHECK(source=="override:"+door);

        // Damaged files and dangling symlinks remain visible failures. Upgrade
        // must not silently switch an engineer's selected calibration.
        { std::ofstream out(door); out << "{}"; }
        { std::ofstream out(plate); out << "{}"; }
        CHECK(!load_cabin_profile(detector,true,nullptr,door,source,error));
        CHECK(source=="station:"+door);
        CHECK(!load_cabin_profile(detector,false,nullptr,plate,source,error));
        CHECK(source=="station:"+plate);
        std::filesystem::remove(door);
#ifdef __linux__
        std::filesystem::create_symlink(missing,door);
        CHECK(!load_cabin_profile(detector,true,nullptr,door,source,error));
        CHECK(source=="station:"+door);
        std::filesystem::remove(door);
#endif
        CHECK(load_cabin_profile(detector,true,"",door,source,error));
        CHECK(source.rfind("bundled:",0)==0);
        std::filesystem::remove_all(directory);
        std::cout << "Bundled profile and strict external override selection passed\n";
        return 0;
    } catch(const std::exception& e) {
        std::filesystem::remove_all(directory);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
