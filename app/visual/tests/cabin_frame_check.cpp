#include "cabin_detector.hpp"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    const bool bundled = argc > 1 && std::string(argv[1]) == "--bundled";
    if(argc < (bundled ? 3 : 4)) {
        std::cerr << "Usage: cabin_frame_check --bundled IMAGE...\n"
                     "   or: cabin_frame_check PLATE_CONFIG DOOR_CONFIG IMAGE...\n";
        return 2;
    }
    cv::setNumThreads(1);
    upc::visual::CabinDetector d; std::string error;
    if(!(bundled ? d.load_bundled_plate_config(error) : d.load_plate_config(argv[1],error))) {
        std::cerr << error << '\n'; return 1;
    }
    if(!(bundled ? d.load_bundled_door_config(error) : d.load_door_config(argv[2],error))) {
        std::cerr << error << '\n'; return 1;
    }
    if(bundled) std::cout << "Bundled profile: " << upc::visual::cabin_bundled_profile_id() << '\n';
    for(int i=(bundled ? 2 : 3);i<argc;++i) {
        const cv::Mat bgr=cv::imread(argv[i]);
        if(bgr.empty() || (bgr.cols&1) || (bgr.rows&1)) return 1;
        cv::Mat i420; cv::cvtColor(bgr,i420,cv::COLOR_BGR2YUV_I420);
        const size_t pixels=bgr.total();
        std::vector<uint8_t> nv12(i420.data,i420.data+pixels*3/2);
        for(size_t j=0;j<pixels/4;++j) { nv12[pixels+2*j]=i420.data[pixels+j]; nv12[pixels+2*j+1]=i420.data[pixels+pixels/4+j]; }
        const auto plate=d.plate(nv12.data(),nv12.size(),bgr.cols,bgr.rows,0x3f);
        const auto door=d.door(nv12.data(),nv12.size(),bgr.cols,bgr.rows,0x3f);
        std::cout << argv[i] << " plate=" << int(plate.result) << " score=" << plate.score
                  << " reason=" << plate.reason << " door=" << int(door.result) << " mean=" << door.score << '\n';
    }
}
