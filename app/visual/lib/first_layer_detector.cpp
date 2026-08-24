#include "first_layer_detector.hpp"

#include "generated_fusion_model.hpp"
#include "generated_rknn_contract.hpp"
#include "rknn_api.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace upc::visual {
namespace {

namespace contract = rknn_first_layer_contract;
using Clock = std::chrono::steady_clock;

double elapsed_ms(const Clock::time_point& begin)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

bool frame_due(uint32_t current, uint32_t target)
{
    return static_cast<int32_t>(current - target) >= 0;
}

std::vector<uint8_t> read_binary(const char* path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if(!stream) {
        throw std::runtime_error(std::string("cannot open RKNN model: ") + path);
    }
    const std::streamoff length = stream.tellg();
    if(length <= 0 || length > static_cast<std::streamoff>(UINT32_MAX)) {
        throw std::runtime_error(std::string("invalid RKNN model size: ") + path);
    }
    stream.seekg(0);
    std::vector<uint8_t> values(static_cast<std::size_t>(length));
    if(!stream.read(reinterpret_cast<char*>(values.data()), length)) {
        throw std::runtime_error(std::string("cannot read RKNN model: ") + path);
    }
    return values;
}

std::string join_reasons(const std::vector<std::string>& reasons)
{
    std::ostringstream stream;
    for(std::size_t index = 0; index < reasons.size(); ++index) {
        if(index != 0) {
            stream << ',';
        }
        stream << reasons[index];
    }
    return stream.str();
}

std::string environment_or_default(const char* name, const char* fallback)
{
    const char* value = std::getenv(name);
    return value && value[0] != '\0' ? value : fallback;
}

float median(std::vector<float> values)
{
    if(values.empty()) {
        return 0.0f;
    }
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    if(values.size() % 2 != 0) {
        return values[middle];
    }
    const float upper = values[middle];
    std::nth_element(values.begin(), values.begin() + middle - 1, values.end());
    return 0.5f * (values[middle - 1] + upper);
}

struct RknnApi {
    using Init = decltype(&rknn_init);
    using Destroy = decltype(&rknn_destroy);
    using Query = decltype(&rknn_query);
    using InputsSet = decltype(&rknn_inputs_set);
    using Run = decltype(&rknn_run);
    using OutputsGet = decltype(&rknn_outputs_get);
    using OutputsRelease = decltype(&rknn_outputs_release);

    void* handle = nullptr;
    Init init = nullptr;
    Destroy destroy = nullptr;
    Query query = nullptr;
    InputsSet inputs_set = nullptr;
    Run run = nullptr;
    OutputsGet outputs_get = nullptr;
    OutputsRelease outputs_release = nullptr;

    RknnApi()
    {
        handle = ::dlopen("/usr/lib/librknnrt.so", RTLD_NOW | RTLD_LOCAL);
        if(!handle) {
            handle = ::dlopen("librknnrt.so", RTLD_NOW | RTLD_LOCAL);
        }
        if(!handle) {
            throw std::runtime_error(std::string("dlopen librknnrt failed: ") + ::dlerror());
        }
        init = symbol<Init>("rknn_init");
        destroy = symbol<Destroy>("rknn_destroy");
        query = symbol<Query>("rknn_query");
        inputs_set = symbol<InputsSet>("rknn_inputs_set");
        run = symbol<Run>("rknn_run");
        outputs_get = symbol<OutputsGet>("rknn_outputs_get");
        outputs_release = symbol<OutputsRelease>("rknn_outputs_release");
    }

    ~RknnApi()
    {
        if(handle) {
            ::dlclose(handle);
        }
    }

private:
    template<typename T>
    T symbol(const char* name)
    {
        ::dlerror();
        void* value = ::dlsym(handle, name);
        const char* error = ::dlerror();
        if(error || !value) {
            throw std::runtime_error(std::string("missing RKNN symbol ") + name);
        }
        return reinterpret_cast<T>(value);
    }
};

class RknnModel {
public:
    explicit RknnModel(RknnApi& api) : api_(api) {}

    ~RknnModel()
    {
        if(context_ != 0) {
            api_.destroy(context_);
        }
    }

    void load(const char* path, uint32_t expected_input_elements,
              uint32_t expected_outputs)
    {
        model_ = read_binary(path);
        check(api_.init(&context_, model_.data(), static_cast<uint32_t>(model_.size()),
                        0, nullptr), "rknn_init");
        rknn_input_output_num io{};
        check(api_.query(context_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)),
              "RKNN_QUERY_IN_OUT_NUM");
        if(io.n_input != 1 || io.n_output != expected_outputs) {
            throw std::runtime_error("unexpected RKNN input/output count");
        }
        input_attr_ = {};
        input_attr_.index = 0;
        check(api_.query(context_, RKNN_QUERY_INPUT_ATTR, &input_attr_,
                         sizeof(input_attr_)), "RKNN_QUERY_INPUT_ATTR");
        if(input_attr_.n_elems != expected_input_elements) {
            std::ostringstream detail;
            detail << "unexpected RKNN physical input size path=" << path
                   << " expected=" << expected_input_elements
                   << " actual=" << input_attr_.n_elems
                   << " n_dims=" << input_attr_.n_dims << " dims=";
            for(uint32_t index = 0; index < input_attr_.n_dims; ++index) {
                if(index != 0) {
                    detail << 'x';
                }
                detail << input_attr_.dims[index];
            }
            detail << " fmt=" << static_cast<int>(input_attr_.fmt)
                   << " type=" << static_cast<int>(input_attr_.type)
                   << " size=" << input_attr_.size
                   << " size_with_stride=" << input_attr_.size_with_stride;
            throw std::runtime_error(detail.str());
        }
        output_attrs_.resize(io.n_output);
        for(uint32_t index = 0; index < io.n_output; ++index) {
            output_attrs_[index] = {};
            output_attrs_[index].index = index;
            check(api_.query(context_, RKNN_QUERY_OUTPUT_ATTR, &output_attrs_[index],
                             sizeof(output_attrs_[index])), "RKNN_QUERY_OUTPUT_ATTR");
        }
    }

    const std::vector<rknn_tensor_attr>& output_attrs() const
    {
        return output_attrs_;
    }

    bool run_uint8_nhwc(const uint8_t* values, std::size_t elements,
                        std::vector<std::vector<float>>& outputs,
                        double& inference_ms, std::string& error)
    {
        if(context_ == 0 || !values || elements != input_attr_.n_elems) {
            error = "invalid RKNN input";
            return false;
        }
        rknn_input input{};
        input.index = 0;
        input.buf = const_cast<uint8_t*>(values);
        input.size = static_cast<uint32_t>(elements);
        input.pass_through = 0;
        input.type = RKNN_TENSOR_UINT8;
        input.fmt = RKNN_TENSOR_NHWC;
        const auto begin = Clock::now();
        int status = api_.inputs_set(context_, 1, &input);
        if(status == RKNN_SUCC) {
            status = api_.run(context_, nullptr);
        }
        std::vector<rknn_output> raw(output_attrs_.size());
        if(status == RKNN_SUCC) {
            for(uint32_t index = 0; index < raw.size(); ++index) {
                raw[index].index = index;
                raw[index].want_float = 1;
                raw[index].is_prealloc = 0;
            }
            status = api_.outputs_get(context_, static_cast<uint32_t>(raw.size()),
                                      raw.data(), nullptr);
        }
        inference_ms = elapsed_ms(begin);
        if(status != RKNN_SUCC) {
            error = "RKNN inference failed: " + std::to_string(status);
            return false;
        }
        outputs.clear();
        outputs.reserve(raw.size());
        for(std::size_t index = 0; index < raw.size(); ++index) {
            const float* data = static_cast<const float*>(raw[index].buf);
            outputs.emplace_back(data, data + output_attrs_[index].n_elems);
        }
        const int release_status = api_.outputs_release(
            context_, static_cast<uint32_t>(raw.size()), raw.data());
        if(release_status != RKNN_SUCC) {
            error = "RKNN output release failed: " + std::to_string(release_status);
            return false;
        }
        return true;
    }

private:
    static void check(int status, const char* operation)
    {
        if(status != RKNN_SUCC) {
            throw std::runtime_error(std::string(operation) + " failed: " +
                                     std::to_string(status));
        }
    }

    RknnApi& api_;
    rknn_context context_ = 0;
    std::vector<uint8_t> model_;
    rknn_tensor_attr input_attr_{};
    std::vector<rknn_tensor_attr> output_attrs_;
};

struct GeometryCandidate {
    std::array<cv::Point2f, 4> corners{};
    float area_ratio = 0.0f;
    float component_fraction = 0.0f;
    float quad_iou = 0.0f;
    float mean_probability = 0.0f;
};

struct GeometryCache {
    uint32_t width = 0;
    uint32_t height = 0;
    std::array<cv::Point2f, 4> corners_320{};
    std::array<cv::Point2f, 4> corners_full{};
    cv::Mat homography;
    float area_ratio = 0.0f;
    uint32_t updated_frame = 0;
};

struct ObserveResult {
    bool ran = false;
    double segmentation_ms = 0.0;
    std::string event = "cache_reused";
    std::string reason = "not_run";
};

std::array<cv::Point2f, 4> order_quad(const std::vector<cv::Point>& points)
{
    if(points.size() != 4) {
        throw std::runtime_error("quadrilateral does not contain four points");
    }
    std::array<cv::Point2f, 4> ordered{};
    auto min_sum = points.begin();
    auto max_sum = points.begin();
    auto min_diff = points.begin();
    auto max_diff = points.begin();
    for(auto iterator = points.begin(); iterator != points.end(); ++iterator) {
        const int sum = iterator->x + iterator->y;
        const int diff = iterator->x - iterator->y;
        if(sum < min_sum->x + min_sum->y) min_sum = iterator;
        if(sum > max_sum->x + max_sum->y) max_sum = iterator;
        if(diff < min_diff->x - min_diff->y) min_diff = iterator;
        if(diff > max_diff->x - max_diff->y) max_diff = iterator;
    }
    ordered[0] = *min_sum;
    ordered[1] = *max_diff;
    ordered[2] = *max_sum;
    ordered[3] = *min_diff;
    return ordered;
}

std::pair<std::array<cv::Point2f, 4>, float> summarize_group(
    const std::vector<GeometryCandidate>& candidates)
{
    std::array<cv::Point2f, 4> corners{};
    for(std::size_t point = 0; point < 4; ++point) {
        std::vector<float> xs;
        std::vector<float> ys;
        for(const auto& candidate : candidates) {
            xs.push_back(candidate.corners[point].x);
            ys.push_back(candidate.corners[point].y);
        }
        corners[point] = cv::Point2f(median(xs), median(ys));
    }
    std::vector<float> areas;
    for(const auto& candidate : candidates) {
        areas.push_back(candidate.area_ratio);
    }
    return {corners, median(areas)};
}

float group_scatter(const std::vector<GeometryCandidate>& candidates,
                    const std::array<cv::Point2f, 4>& center)
{
    float maximum = 0.0f;
    for(const auto& candidate : candidates) {
        for(std::size_t point = 0; point < 4; ++point) {
            maximum = std::max(maximum,
                static_cast<float>(cv::norm(candidate.corners[point] - center[point])));
        }
    }
    return maximum;
}

cv::Mat make_nv12_view(const uint8_t* nv12, std::size_t bytes,
                       uint32_t width, uint32_t height)
{
    const std::size_t expected = static_cast<std::size_t>(width) * height * 3u / 2u;
    if(!nv12 || width == 0 || height == 0 || (width % 2u) != 0 ||
       (height % 2u) != 0 || bytes != expected) {
        throw std::runtime_error("invalid tight NV12 frame");
    }
    return cv::Mat(static_cast<int>(height * 3u / 2u), static_cast<int>(width),
                   CV_8UC1, const_cast<uint8_t*>(nv12));
}

cv::Mat make_gray_view(const uint8_t* nv12, uint32_t width, uint32_t height)
{
    return cv::Mat(static_cast<int>(height), static_cast<int>(width), CV_8UC1,
                   const_cast<uint8_t*>(nv12));
}

struct SceneQuality {
    bool passed = false;
    std::vector<std::string> reasons;
};

SceneQuality check_scene_quality(const cv::Mat& gray)
{
    std::array<uint32_t, 256> histogram{};
    uint64_t under = 0;
    uint64_t over = 0;
    for(int row = 0; row < gray.rows; ++row) {
        const uint8_t* values = gray.ptr<uint8_t>(row);
        for(int column = 0; column < gray.cols; ++column) {
            const uint8_t value = values[column];
            ++histogram[value];
            under += value <= fusion_model::kUnderexposedThreshold;
            over += value >= fusion_model::kOverexposedThreshold;
        }
    }
    const uint64_t count = gray.total();
    const auto percentile = [&](double fraction) {
        const uint64_t target = static_cast<uint64_t>(fraction * (count - 1));
        uint64_t accumulated = 0;
        for(int value = 0; value < 256; ++value) {
            accumulated += histogram[static_cast<std::size_t>(value)];
            if(accumulated > target) return static_cast<float>(value);
        }
        return 255.0f;
    };
    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(gray, mean, deviation);
    SceneQuality quality;
    if(static_cast<double>(under) / count > fusion_model::kMaxUnderexposedRatio) {
        quality.reasons.emplace_back("excessive_underexposure");
    }
    if(static_cast<double>(over) / count > fusion_model::kMaxOverexposedRatio) {
        quality.reasons.emplace_back("excessive_overexposure");
    }
    if(percentile(0.95) - percentile(0.05) < fusion_model::kMinP05P95Range) {
        quality.reasons.emplace_back("low_dynamic_range");
    }
    if(deviation[0] < fusion_model::kMinGrayStd) {
        quality.reasons.emplace_back("low_texture_information");
    }
    quality.passed = quality.reasons.empty();
    return quality;
}

cv::Mat fixed_signed_u8(const cv::Mat& values, float limit)
{
    cv::Mat output(values.size(), CV_8UC1);
    for(int row = 0; row < values.rows; ++row) {
        const float* input = values.ptr<float>(row);
        uint8_t* target = output.ptr<uint8_t>(row);
        for(int column = 0; column < values.cols; ++column) {
            const float normalized = std::max(-1.0f, std::min(1.0f, input[column] / limit));
            target[column] = cv::saturate_cast<uint8_t>(
                std::round((normalized + 1.0f) * 127.5f));
        }
    }
    return output;
}

cv::Mat fixed_unsigned_u8(const cv::Mat& values, float limit)
{
    cv::Mat output(values.size(), CV_8UC1);
    for(int row = 0; row < values.rows; ++row) {
        const float* input = values.ptr<float>(row);
        uint8_t* target = output.ptr<uint8_t>(row);
        for(int column = 0; column < values.cols; ++column) {
            const float normalized = std::max(0.0f, std::min(1.0f, input[column] / limit));
            target[column] = cv::saturate_cast<uint8_t>(std::round(normalized * 255.0f));
        }
    }
    return output;
}

cv::Mat fixed_centered_u8(const cv::Mat& values, float limit)
{
    cv::Mat output(values.size(), CV_8UC1);
    for(int row = 0; row < values.rows; ++row) {
        const float* input = values.ptr<float>(row);
        uint8_t* target = output.ptr<uint8_t>(row);
        for(int column = 0; column < values.cols; ++column) {
            const float normalized = std::max(-1.0f, std::min(1.0f, input[column] / limit));
            target[column] = cv::saturate_cast<uint8_t>(
                std::round(128.0f + normalized * 127.0f));
        }
    }
    return output;
}

cv::Mat build_fusion_input(const cv::Mat& canonical_gray,
                           const cv::Mat& canonical_bare)
{
    if(canonical_gray.empty() || canonical_bare.empty() ||
       canonical_gray.type() != CV_8UC1 || canonical_bare.type() != CV_8UC1 ||
       canonical_gray.size() != canonical_bare.size()) {
        throw std::runtime_error("invalid canonical bare-reference pair");
    }
    cv::Mat gray_float;
    canonical_gray.convertTo(gray_float, CV_32F);
    cv::Mat log_values;
    gray_float.copyTo(log_values);
    log_values += fusion_model::kLogFloor;
    cv::log(log_values, log_values);
    cv::Mat fine_illumination;
    cv::GaussianBlur(log_values, fine_illumination, cv::Size(), 2.0, 2.0,
                     cv::BORDER_REFLECT_101);

    cv::Mat low_frequency;
    const double low_sigma = std::max(
        static_cast<double>(canonical_gray.cols) /
            fusion_model::kLowFrequencySigmaDivisor,
        1.0);
    cv::GaussianBlur(gray_float, low_frequency, cv::Size(), low_sigma, low_sigma,
                     cv::BORDER_REFLECT_101);
    low_frequency += fusion_model::kLogFloor;
    cv::log(low_frequency, low_frequency);

    cv::Mat gradient_x;
    cv::Mat gradient_y;
    cv::Mat gradient;
    cv::Sobel(log_values, gradient_x, CV_32F, 1, 0, 3);
    cv::Sobel(log_values, gradient_y, CV_32F, 0, 1, 3);
    cv::magnitude(gradient_x, gradient_y, gradient);

    cv::Mat bare_log;
    canonical_bare.convertTo(bare_log, CV_32F);
    bare_log += fusion_model::kLogFloor;
    cv::log(bare_log, bare_log);

    std::vector<cv::Mat> channels{
        canonical_gray,
        fixed_signed_u8(log_values - fine_illumination, 0.12f),
        fixed_centered_u8(log_values - low_frequency,
                          fusion_model::kLowFrequencyLogLimit),
        fixed_unsigned_u8(gradient, 0.90f),
        fixed_signed_u8(log_values - bare_log,
                        fusion_model::kReferenceChangeLogLimit),
    };
    std::vector<cv::Mat> resized_channels;
    resized_channels.reserve(channels.size());
    for(const cv::Mat& channel : channels) {
        cv::Mat resized;
        cv::resize(channel, resized,
                   cv::Size(fusion_model::kInputSize, fusion_model::kInputSize),
                   0.0, 0.0, cv::INTER_AREA);
        resized_channels.push_back(std::move(resized));
    }
    cv::Mat merged;
    cv::merge(resized_channels, merged);
    return merged.isContinuous() ? merged : merged.clone();
}

} // namespace

class FirstLayerDetector::Impl {
public:
    Impl()
        : segmentation_path_(environment_or_default(
              "FIRST_LAYER_RKNN_SEGMENTATION_MODEL", contract::kSegmentationPath)),
          fusion_path_(environment_or_default(
              "FIRST_LAYER_RKNN_FUSION_MODEL", contract::kFusionPath)),
          cache_path_(environment_or_default(
              "FIRST_LAYER_RKNN_CACHE_PATH", contract::kCachePath))
    {}

    bool geometry_due(uint32_t frame_seq) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !runtime_ready_ || frame_due(frame_seq, next_segmentation_frame_);
    }

    FirstLayerGeometryObservation observe(
        const uint8_t* nv12, std::size_t bytes,
        uint32_t width, uint32_t height, uint32_t frame_seq)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        FirstLayerGeometryObservation output;
        try {
            ensure_runtime();
            ensure_cache_loaded(width, height, frame_seq);
            const ObserveResult observation = observe_locked(
                nv12, bytes, width, height, frame_seq);
            last_cache_event_ = observation.event;
            last_segmentation_reason_ = observation.reason;
            output.segmentation_ran = observation.ran;
            output.segmentation_ms = observation.segmentation_ms;
            output.event = observation.event;
            output.reason = observation.reason;
        } catch(const std::exception& error) {
            last_cache_event_ = "segmentation_exception";
            last_segmentation_reason_ = error.what();
            next_segmentation_frame_ = frame_seq + contract::kFailedRetryFrames;
            output.segmentation_ran = true;
            output.event = last_cache_event_;
            output.reason = last_segmentation_reason_;
        }
        output.cache_available = cache_available_;
        return output;
    }

    bool capture_bare_reference(const uint8_t* nv12, std::size_t bytes,
                                uint32_t width, uint32_t height,
                                uint32_t frame_seq, std::string& reason)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        try {
            make_nv12_view(nv12, bytes, width, height);
            ensure_runtime();
            ensure_cache_loaded(width, height, frame_seq);
            const ObserveResult observation = observe_locked(
                nv12, bytes, width, height, frame_seq);
            last_cache_event_ = observation.event;
            last_segmentation_reason_ = observation.reason;
            if(!cache_available_) {
                reason = "geometry_cache_unavailable:" + last_segmentation_reason_;
                return false;
            }
            cv::Mat canonical;
            cv::warpPerspective(make_gray_view(nv12, width, height), canonical,
                                cache_.homography,
                                cv::Size(contract::kCanonicalSize,
                                         contract::kCanonicalSize),
                                cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
            const SceneQuality quality = check_scene_quality(canonical);
            if(!quality.passed) {
                reason = "bare_reference_scene_quality:" +
                    join_reasons(quality.reasons);
                return false;
            }
            bare_reference_gray_ = make_gray_view(nv12, width, height).clone();
            bare_reference_width_ = width;
            bare_reference_height_ = height;
            bare_reference_frame_seq_ = frame_seq;
            reason = "ok";
            return true;
        } catch(const std::exception& error) {
            reason = error.what();
            return false;
        }
    }

    void clear_bare_reference()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bare_reference_gray_.release();
        bare_reference_width_ = 0;
        bare_reference_height_ = 0;
        bare_reference_frame_seq_ = 0;
    }

    bool has_bare_reference() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return !bare_reference_gray_.empty();
    }

    FirstLayerDetection predict(const uint8_t* nv12, std::size_t bytes,
                                uint32_t width, uint32_t height,
                                uint32_t frame_seq)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        FirstLayerDetection result;
        const auto total_begin = Clock::now();
        try {
            make_nv12_view(nv12, bytes, width, height);
            ensure_runtime();
            ensure_cache_loaded(width, height, frame_seq);
            const ObserveResult observation = observe_locked(
                nv12, bytes, width, height, frame_seq);
            result.segmentation_ran = observation.ran;
            result.segmentation_ms = observation.segmentation_ms;
            last_cache_event_ = observation.event;
            last_segmentation_reason_ = observation.reason;
            result.cache_event = last_cache_event_;
            result.cache_available = cache_available_;
            if(!cache_available_) {
                throw std::runtime_error(
                    "geometry_cache_unavailable:" + last_segmentation_reason_);
            }
            result.alignment_passed = true;
            const auto preprocessing_begin = Clock::now();
            const cv::Mat gray = make_gray_view(nv12, width, height);
            cv::Mat canonical;
            cv::warpPerspective(gray, canonical, cache_.homography,
                                cv::Size(contract::kCanonicalSize,
                                         contract::kCanonicalSize),
                                cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
            if(bare_reference_gray_.empty() ||
               bare_reference_width_ != width ||
               bare_reference_height_ != height) {
                throw std::runtime_error("bare_reference_unavailable");
            }
            cv::Mat canonical_bare;
            cv::warpPerspective(bare_reference_gray_, canonical_bare,
                                cache_.homography,
                                cv::Size(contract::kCanonicalSize,
                                         contract::kCanonicalSize),
                                cv::INTER_LINEAR, cv::BORDER_REFLECT_101);
            SceneQuality quality = check_scene_quality(canonical);
            cv::Mat fusion_input = build_fusion_input(canonical, canonical_bare);
            result.preprocessing_ms = elapsed_ms(preprocessing_begin);

            std::vector<std::vector<float>> outputs;
            std::string inference_error;
            if(!fusion_model_instance().run_uint8_nhwc(
                   fusion_input.ptr<uint8_t>(),
                   fusion_input.total() * fusion_model::kInputChannels,
                   outputs, result.fusion_ms, inference_error)) {
                throw std::runtime_error(inference_error);
            }
            if(outputs.size() != 2 || outputs[0].size() != 2 ||
               outputs[1].size() != fusion_model::kEmbeddingSize) {
                throw std::runtime_error("unexpected fusion RKNN outputs");
            }
            const float maximum = std::max(outputs[0][0], outputs[0][1]);
            const double exp0 = std::exp(static_cast<double>(outputs[0][0] - maximum));
            const double exp1 = std::exp(static_cast<double>(outputs[0][1] - maximum));
            result.ready_probability = exp1 / (exp0 + exp1);
            result.confidence = std::max(result.ready_probability,
                                         1.0 - result.ready_probability);
            result.raw_class = result.ready_probability >= 0.5
                ? "ready_for_first_layer" : "not_ready_for_first_layer";
            double squared = 0.0;
            for(std::size_t index = 0; index < outputs[1].size(); ++index) {
                const double standardized =
                    (outputs[1][index] - fusion_model::kOodMean[index]) /
                    fusion_model::kOodScale[index];
                squared += standardized * standardized;
            }
            result.ood_distance = std::sqrt(squared / outputs[1].size());
            result.is_ood = result.ood_distance > fusion_model::kOodThreshold;
            std::vector<std::string> reasons = std::move(quality.reasons);
            if(result.confidence < fusion_model::kConfidenceThreshold) {
                reasons.emplace_back("low_confidence");
            }
            if(result.is_ood) {
                reasons.emplace_back("ood");
            }
            if(reasons.empty()) {
                result.output_class = result.raw_class;
                if(result.output_class == "ready_for_first_layer" &&
                   result.ready_probability < fusion_model::kReadyThreshold) {
                    result.output_class = "not_ready_for_first_layer";
                    reasons.emplace_back("ready_probability_below_threshold");
                }
            } else {
                result.output_class = "uncertain";
            }
            result.rejection_reasons = join_reasons(reasons);
            result.passed = result.output_class == "ready_for_first_layer";
            /* 通过与最终拦截诊断都需复用同一张透视矫正 ROI，避免复核图
             * 与模型实际输入不一致。只有几何校正成功的正常推理会走到这里。 */
            if(result.alignment_passed) {
                const cv::Mat canonical_tight = canonical.isContinuous()
                    ? canonical : canonical.clone();
                result.canonical_width = static_cast<uint32_t>(canonical_tight.cols);
                result.canonical_height = static_cast<uint32_t>(canonical_tight.rows);
                result.canonical_gray.assign(
                    canonical_tight.ptr<uint8_t>(),
                    canonical_tight.ptr<uint8_t>() + canonical_tight.total());
            }
        } catch(const std::exception& error) {
            result.error = error.what();
            result.passed = false;
            result.output_class = "uncertain";
            result.cache_available = cache_available_;
            result.cache_event = last_cache_event_;
        }
        result.total_ms = elapsed_ms(total_begin);
        return result;
    }

private:
    void ensure_runtime()
    {
        if(runtime_ready_) {
            return;
        }
        fusion_model_holder_.reset();
        segmentation_model_.reset();
        api_.reset();
        auto api = std::make_unique<RknnApi>();
        auto segmentation = std::make_unique<RknnModel>(*api);
        auto fusion = std::make_unique<RknnModel>(*api);
        segmentation->load(
            segmentation_path_.c_str(),
            contract::kSegmentationSize * contract::kSegmentationSize * 3u, 1);
        fusion->load(
            fusion_path_.c_str(),
            fusion_model::kInputSize * fusion_model::kInputSize *
                fusion_model::kInputChannels, 2);
        api_ = std::move(api);
        segmentation_model_ = std::move(segmentation);
        fusion_model_holder_ = std::move(fusion);
        runtime_ready_ = true;
    }

    RknnModel& segmentation_model()
    {
        return *segmentation_model_;
    }

    RknnModel& fusion_model_instance()
    {
        return *fusion_model_holder_;
    }

    void ensure_cache_loaded(uint32_t width, uint32_t height, uint32_t frame_seq)
    {
        if(cache_available_ && (cache_.width != width || cache_.height != height)) {
            cache_available_ = false;
            cache_load_attempted_ = false;
            candidates_.clear();
            pending_candidates_.clear();
            pending_available_ = false;
            next_segmentation_frame_ = frame_seq;
        }
        if(cache_load_attempted_) {
            return;
        }
        cache_load_attempted_ = true;
        cv::FileStorage storage(cache_path_, cv::FileStorage::READ);
        if(!storage.isOpened()) {
            return;
        }
        std::string version;
        std::string segmentation_hash;
        int stored_width = 0;
        int stored_height = 0;
        int canonical_size = 0;
        float inset_scale = 0.0f;
        cv::Mat corners;
        cv::Mat homography;
        storage["version"] >> version;
        storage["segmentation_sha256"] >> segmentation_hash;
        storage["width"] >> stored_width;
        storage["height"] >> stored_height;
        storage["canonical_size"] >> canonical_size;
        storage["inset_scale"] >> inset_scale;
        storage["area_ratio"] >> cache_.area_ratio;
        int updated_frame = 0;
        storage["updated_frame"] >> updated_frame;
        cache_.updated_frame = static_cast<uint32_t>(std::max(updated_frame, 0));
        storage["corners_320"] >> corners;
        storage["homography"] >> homography;
        if(version != contract::kContractVersion ||
           segmentation_hash != contract::kSegmentationSha256 ||
           stored_width != static_cast<int>(width) ||
           stored_height != static_cast<int>(height) ||
           canonical_size != contract::kCanonicalSize ||
           std::abs(inset_scale - contract::kInsetScale) > 1e-6f ||
           corners.rows != 4 || corners.cols != 2 || homography.rows != 3 ||
           homography.cols != 3) {
            return;
        }
        corners.convertTo(corners, CV_32F);
        homography.convertTo(cache_.homography, CV_64F);
        cache_.width = width;
        cache_.height = height;
        for(int index = 0; index < 4; ++index) {
            cache_.corners_320[index] = cv::Point2f(
                corners.at<float>(index, 0), corners.at<float>(index, 1));
            cache_.corners_full[index] = cv::Point2f(
                cache_.corners_320[index].x * width / contract::kSegmentationSize,
                cache_.corners_320[index].y * height / contract::kSegmentationSize);
        }
        cache_available_ = cv::checkRange(cache_.homography);
        if(cache_available_) {
            next_segmentation_frame_ = frame_seq + contract::kAuditIntervalFrames;
            last_cache_event_ = "cache_loaded";
        }
    }

    void save_cache()
    {
        const std::string temporary = cache_path_ + ".tmp";
        cv::FileStorage storage(temporary, cv::FileStorage::WRITE);
        if(!storage.isOpened()) {
            return;
        }
        cv::Mat corners(4, 2, CV_32F);
        for(int index = 0; index < 4; ++index) {
            corners.at<float>(index, 0) = cache_.corners_320[index].x;
            corners.at<float>(index, 1) = cache_.corners_320[index].y;
        }
        storage << "version" << contract::kContractVersion;
        storage << "segmentation_sha256" << contract::kSegmentationSha256;
        storage << "fusion_sha256" << contract::kFusionSha256;
        storage << "width" << static_cast<int>(cache_.width);
        storage << "height" << static_cast<int>(cache_.height);
        storage << "canonical_size" << contract::kCanonicalSize;
        storage << "inset_scale" << contract::kInsetScale;
        storage << "area_ratio" << cache_.area_ratio;
        storage << "updated_frame" << static_cast<int>(cache_.updated_frame);
        storage << "corners_320" << corners;
        storage << "homography" << cache_.homography;
        storage.release();
        if(std::rename(temporary.c_str(), cache_path_.c_str()) != 0) {
            std::remove(temporary.c_str());
        }
    }

    ObserveResult observe_locked(const uint8_t* nv12, std::size_t bytes,
                                 uint32_t width, uint32_t height,
                                 uint32_t frame_seq)
    {
        ObserveResult result;
        if(!frame_due(frame_seq, next_segmentation_frame_)) {
            result.event = "cache_reused";
            return result;
        }
        result.ran = true;
        if(!audit_active_) {
            audit_active_ = true;
            audit_started_frame_ = frame_seq;
        }
        GeometryCandidate candidate;
        std::string reason;
        const auto begin = Clock::now();
        const bool valid = infer_candidate(nv12, bytes, width, height,
                                           candidate, reason);
        result.segmentation_ms = elapsed_ms(begin);
        result.reason = reason;
        if(!valid) {
            candidates_.clear();
            next_segmentation_frame_ = frame_seq +
                (reason == "scene_too_dark" ? contract::kDarkSceneRetryFrames
                                             : contract::kFailedRetryFrames);
            result.event = cache_available_ ? "segmentation_failed_cache_reused"
                                            : "startup_failed";
            if(cache_available_ &&
               static_cast<uint32_t>(frame_seq - audit_started_frame_) >=
                   contract::kMaxAuditSpanFrames) {
                finish_audit(frame_seq);
                result.event = "audit_expired_cache_reused";
            }
            return result;
        }
        result.event = accept_candidate(candidate, width, height, frame_seq);
        return result;
    }

    bool infer_candidate(const uint8_t* nv12, std::size_t bytes,
                         uint32_t width, uint32_t height,
                         GeometryCandidate& candidate, std::string& reason)
    {
        if(cv::mean(make_gray_view(nv12, width, height))[0] <
           contract::kSegmentationMinGrayMean) {
            reason = "scene_too_dark";
            return false;
        }
        cv::Mat bgr;
        cv::cvtColor(make_nv12_view(nv12, bytes, width, height), bgr,
                     cv::COLOR_YUV2BGR_NV12);
        cv::Mat resized;
        cv::resize(bgr, resized,
                   cv::Size(contract::kSegmentationSize,
                            contract::kSegmentationSize),
                   0.0, 0.0, cv::INTER_LINEAR);
        cv::Mat rgb;
        cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
        if(!rgb.isContinuous()) rgb = rgb.clone();
        std::vector<std::vector<float>> outputs;
        std::string error;
        double runtime_ms = 0.0;
        if(!segmentation_model().run_uint8_nhwc(
               rgb.ptr<uint8_t>(), rgb.total() * 3u,
               outputs, runtime_ms, error)) {
            reason = error;
            return false;
        }
        if(outputs.size() != 1 ||
           outputs[0].size() != static_cast<std::size_t>(
               2 * contract::kSegmentationSize * contract::kSegmentationSize)) {
            reason = "unexpected_segmentation_output";
            return false;
        }
        cv::Mat probability(contract::kSegmentationSize,
                            contract::kSegmentationSize, CV_32F);
        const auto& attr = segmentation_model().output_attrs()[0];
        const std::size_t plane = probability.total();
        for(std::size_t index = 0; index < plane; ++index) {
            float logit0 = 0.0f;
            float logit1 = 0.0f;
            if(attr.fmt == RKNN_TENSOR_NHWC) {
                logit0 = outputs[0][index * 2u];
                logit1 = outputs[0][index * 2u + 1u];
            } else {
                logit0 = outputs[0][index];
                logit1 = outputs[0][plane + index];
            }
            const float maximum = std::max(logit0, logit1);
            const float value0 = std::exp(logit0 - maximum);
            const float value1 = std::exp(logit1 - maximum);
            probability.ptr<float>()[index] = value1 / (value0 + value1);
        }
        cv::Mat mask;
        cv::compare(probability, 0.5f, mask, cv::CMP_GE);
        const int foreground = cv::countNonZero(mask);
        if(foreground == 0) {
            reason = "empty_mask";
            return false;
        }
        cv::Mat labels;
        cv::Mat stats;
        cv::Mat centroids;
        const int label_count = cv::connectedComponentsWithStats(
            mask, labels, stats, centroids, 8, CV_32S);
        if(label_count <= 1) {
            reason = "no_component";
            return false;
        }
        int largest_label = 1;
        int largest_area = stats.at<int>(1, cv::CC_STAT_AREA);
        for(int label = 2; label < label_count; ++label) {
            const int area = stats.at<int>(label, cv::CC_STAT_AREA);
            if(area > largest_area) {
                largest_area = area;
                largest_label = label;
            }
        }
        cv::Mat component;
        cv::compare(labels, largest_label, component, cv::CMP_EQ);
        const float component_fraction = static_cast<float>(largest_area) / foreground;
        const float area_ratio = static_cast<float>(largest_area) / mask.total();
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(component.clone(), contours, cv::RETR_EXTERNAL,
                         cv::CHAIN_APPROX_SIMPLE);
        if(contours.empty()) {
            reason = "no_contour";
            return false;
        }
        const auto largest = std::max_element(
            contours.begin(), contours.end(), [](const auto& left, const auto& right) {
                return cv::contourArea(left) < cv::contourArea(right);
            });
        std::vector<cv::Point> hull;
        cv::convexHull(*largest, hull);
        const double perimeter = cv::arcLength(hull, true);
        float best_iou = -1.0f;
        std::array<cv::Point2f, 4> best_corners{};
        for(int step = 0; step <= 76; ++step) {
            const double epsilon_ratio = 0.004 + 0.001 * step;
            std::vector<cv::Point> approximation;
            cv::approxPolyDP(hull, approximation, epsilon_ratio * perimeter, true);
            if(approximation.size() != 4 || !cv::isContourConvex(approximation)) {
                continue;
            }
            const auto corners = order_quad(approximation);
            std::vector<cv::Point> polygon;
            for(const auto& point : corners) {
                polygon.emplace_back(cvRound(point.x), cvRound(point.y));
            }
            cv::Mat quad = cv::Mat::zeros(component.size(), CV_8U);
            cv::fillConvexPoly(quad, polygon, cv::Scalar(255));
            cv::Mat intersection;
            cv::Mat union_mask;
            cv::bitwise_and(component, quad, intersection);
            cv::bitwise_or(component, quad, union_mask);
            const float iou = static_cast<float>(cv::countNonZero(intersection)) /
                              std::max(1, cv::countNonZero(union_mask));
            if(iou > best_iou) {
                best_iou = iou;
                best_corners = corners;
            }
        }
        if(best_iou < 0.0f) {
            reason = "no_quadrilateral";
            return false;
        }
        const float mean_probability = static_cast<float>(cv::mean(probability, component)[0]);
        std::vector<std::string> failures;
        if(area_ratio < contract::kAreaRatioMin || area_ratio > contract::kAreaRatioMax)
            failures.emplace_back("area_ratio");
        if(cache_available_ &&
           std::abs(area_ratio / cache_.area_ratio - 1.0f) >
               contract::kCachedAreaRelativeTolerance)
            failures.emplace_back("cached_area_delta");
        if(component_fraction < contract::kComponentFractionMin)
            failures.emplace_back("fragmented");
        if(best_iou < contract::kQuadIouMin)
            failures.emplace_back("quad_fit");
        if(mean_probability < contract::kMeanProbabilityMin)
            failures.emplace_back("low_probability");
        if(!failures.empty()) {
            reason = join_reasons(failures);
            return false;
        }
        candidate.corners = best_corners;
        candidate.area_ratio = area_ratio;
        candidate.component_fraction = component_fraction;
        candidate.quad_iou = best_iou;
        candidate.mean_probability = mean_probability;
        reason = "ok";
        return true;
    }

    std::string accept_candidate(const GeometryCandidate& candidate,
                                 uint32_t width, uint32_t height,
                                 uint32_t frame_seq)
    {
        candidates_.push_back(candidate);
        const int required = cache_available_ ? contract::kProposalValidFrames
                                              : contract::kInitialValidFrames;
        if(static_cast<int>(candidates_.size()) < required) {
            next_segmentation_frame_ = frame_seq + 1;
            return "collecting_candidates";
        }
        auto summary = summarize_group(candidates_);
        const float scatter = group_scatter(candidates_, summary.first);
        if(scatter > contract::kGroupScatterMax320) {
            const GeometryCandidate latest = candidates_.back();
            candidates_.assign(1, latest);
            next_segmentation_frame_ = frame_seq + contract::kFailedRetryFrames;
            return "unstable_candidate_group";
        }
        if(!cache_available_) {
            install_cache(summary.first, summary.second, width, height, frame_seq);
            finish_audit(frame_seq);
            return "cache_initialized";
        }
        cv::Point2f coherent(0.0f, 0.0f);
        std::vector<float> dx;
        std::vector<float> dy;
        float max_corner = 0.0f;
        for(std::size_t point = 0; point < 4; ++point) {
            const cv::Point2f difference = summary.first[point] - cache_.corners_320[point];
            dx.push_back(difference.x);
            dy.push_back(difference.y);
            max_corner = std::max(max_corner,
                static_cast<float>(cv::norm(difference)));
        }
        coherent = cv::Point2f(median(dx), median(dy));
        const float translation = static_cast<float>(cv::norm(coherent));
        const bool clearly_stable =
            translation <= contract::kNoMoveTranslation320 &&
            max_corner <= contract::kNoMoveMaxCorner320;
        const bool clearly_moved =
            translation > contract::kMoveTranslation320 ||
            max_corner > contract::kMoveMaxCorner320;
        if(!pending_available_) {
            if(clearly_stable || !clearly_moved) {
                finish_audit(frame_seq);
                return "cache_stable";
            }
            pending_available_ = true;
            pending_proposal_ = summary.first;
            pending_candidates_ = candidates_;
            candidates_.clear();
            next_segmentation_frame_ = frame_seq + 1;
            return "movement_needs_confirmation";
        }
        float proposal_delta = 0.0f;
        for(std::size_t point = 0; point < 4; ++point) {
            proposal_delta = std::max(proposal_delta,
                static_cast<float>(cv::norm(summary.first[point] - pending_proposal_[point])));
        }
        if(proposal_delta <= contract::kProposalAgreement320) {
            std::vector<GeometryCandidate> all = pending_candidates_;
            all.insert(all.end(), candidates_.begin(), candidates_.end());
            auto final_summary = summarize_group(all);
            if(group_scatter(all, final_summary.first) <= contract::kGroupScatterMax320) {
                install_cache(final_summary.first, final_summary.second,
                              width, height, frame_seq);
                finish_audit(frame_seq);
                return "cache_updated";
            }
        }
        finish_audit(frame_seq);
        return "movement_confirmation_rejected";
    }

    void install_cache(const std::array<cv::Point2f, 4>& corners,
                       float area_ratio, uint32_t width, uint32_t height,
                       uint32_t frame_seq)
    {
        cache_.width = width;
        cache_.height = height;
        cache_.corners_320 = corners;
        cache_.area_ratio = area_ratio;
        cache_.updated_frame = frame_seq;
        std::array<cv::Point2f, 4> source{};
        cv::Point2f center(0.0f, 0.0f);
        for(std::size_t point = 0; point < 4; ++point) {
            cache_.corners_full[point] = cv::Point2f(
                corners[point].x * width / contract::kSegmentationSize,
                corners[point].y * height / contract::kSegmentationSize);
            center += cache_.corners_full[point];
        }
        center *= 0.25f;
        for(std::size_t point = 0; point < 4; ++point) {
            source[point] = center +
                (cache_.corners_full[point] - center) * contract::kInsetScale;
        }
        const float edge = static_cast<float>(contract::kCanonicalSize - 1);
        const std::array<cv::Point2f, 4> target{
            cv::Point2f(0, 0), cv::Point2f(edge, 0),
            cv::Point2f(edge, edge), cv::Point2f(0, edge)};
        cache_.homography = cv::getPerspectiveTransform(source.data(), target.data());
        if(!cv::checkRange(cache_.homography) ||
           std::abs(cv::determinant(cache_.homography)) < 1e-12) {
            throw std::runtime_error("invalid segmentation-derived homography");
        }
        cache_available_ = true;
        save_cache();
    }

    void finish_audit(uint32_t frame_seq)
    {
        candidates_.clear();
        pending_candidates_.clear();
        pending_available_ = false;
        audit_active_ = false;
        next_segmentation_frame_ = frame_seq + contract::kAuditIntervalFrames;
    }

    mutable std::mutex mutex_;
    const std::string segmentation_path_;
    const std::string fusion_path_;
    const std::string cache_path_;
    bool runtime_ready_ = false;
    std::unique_ptr<RknnApi> api_;
    std::unique_ptr<RknnModel> segmentation_model_;
    std::unique_ptr<RknnModel> fusion_model_holder_;

    bool cache_load_attempted_ = false;
    bool cache_available_ = false;
    GeometryCache cache_;
    uint32_t next_segmentation_frame_ = 0;
    bool audit_active_ = false;
    uint32_t audit_started_frame_ = 0;
    std::vector<GeometryCandidate> candidates_;
    bool pending_available_ = false;
    std::array<cv::Point2f, 4> pending_proposal_{};
    std::vector<GeometryCandidate> pending_candidates_;
    std::string last_cache_event_ = "not_run";
    std::string last_segmentation_reason_ = "not_run";

    cv::Mat bare_reference_gray_;
    uint32_t bare_reference_width_ = 0;
    uint32_t bare_reference_height_ = 0;
    uint32_t bare_reference_frame_seq_ = 0;

};

FirstLayerDetector::FirstLayerDetector() : impl_(std::make_unique<Impl>()) {}
FirstLayerDetector::~FirstLayerDetector() = default;

bool FirstLayerDetector::geometry_observation_due(uint32_t frame_seq) const
{
    return impl_->geometry_due(frame_seq);
}

FirstLayerGeometryObservation FirstLayerDetector::observe_nv12(
    const uint8_t* nv12, std::size_t bytes, uint32_t width, uint32_t height,
    uint32_t frame_seq)
{
    return impl_->observe(nv12, bytes, width, height, frame_seq);
}

bool FirstLayerDetector::capture_bare_reference_nv12(
    const uint8_t* nv12, std::size_t bytes, uint32_t width, uint32_t height,
    uint32_t frame_seq, std::string& reason)
{
    return impl_->capture_bare_reference(
        nv12, bytes, width, height, frame_seq, reason);
}

void FirstLayerDetector::clear_bare_reference()
{
    impl_->clear_bare_reference();
}

bool FirstLayerDetector::has_bare_reference() const
{
    return impl_->has_bare_reference();
}

FirstLayerDetection FirstLayerDetector::predict_nv12(
    const uint8_t* nv12, std::size_t bytes, uint32_t width, uint32_t height,
    uint32_t frame_seq)
{
    return impl_->predict(nv12, bytes, width, height, frame_seq);
}

} // namespace upc::visual
