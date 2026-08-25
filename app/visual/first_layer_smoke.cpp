#include "first_layer_detector.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char** argv)
{
    if(argc != 4 && argc != 5) {
        std::cerr << "usage: rknn_first_layer_smoke gray.raw width height [prime_frames]\n";
        return 2;
    }
    const uint32_t width = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const uint32_t height = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10));
    const std::size_t y_bytes = static_cast<std::size_t>(width) * height;
    std::vector<uint8_t> nv12(y_bytes * 3u / 2u, 128u);
    std::ifstream stream(argv[1], std::ios::binary);
    if(!stream.read(reinterpret_cast<char*>(nv12.data()),
                    static_cast<std::streamsize>(y_bytes))) {
        std::cerr << "cannot read full gray input\n";
        return 2;
    }
    upc::visual::FirstLayerDetector detector;
    const int prime_frames = argc == 5 ? std::max(0, std::atoi(argv[4])) : 0;
    for(int index = 0; index < prime_frames; ++index) {
        detector.observe_nv12(nv12.data(), nv12.size(), width, height,
                              static_cast<uint32_t>(1000 + index));
    }
    const auto result = detector.predict_nv12(
        nv12.data(), nv12.size(), width, height,
        static_cast<uint32_t>(1000 + prime_frames));
    std::cout
        << "error=" << result.error << '\n'
        << "cache_event=" << result.cache_event << '\n'
        << "alignment=" << (result.alignment_passed ? 1 : 0) << '\n'
        << "raw_class=" << result.raw_class << '\n'
        << "output_class=" << result.output_class << '\n'
        << "ready_probability=" << result.ready_probability << '\n'
        << "confidence=" << result.confidence << '\n'
        << "ood_distance=" << result.ood_distance << '\n'
        << "rejection_reasons=" << result.rejection_reasons << '\n'
        << "preprocessing_ms=" << result.preprocessing_ms << '\n'
        << "fusion_ms=" << result.fusion_ms << '\n'
        << "total_ms=" << result.total_ms << '\n'
        << "result=" << (result.passed ? 1 : 0) << '\n';
    return result.error.empty() ? 0 : 3;
}
