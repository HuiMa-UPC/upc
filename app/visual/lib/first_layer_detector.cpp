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
    float mask_coverage = 0.0f;
    float quad_fill = 0.0f;
    float edge_residual = 0.0f;
    float edge_inlier_fraction = 0.0f;
    float mean_probability = 0.0f;
    bool edge_snap_applied = false;
    std::array<float, 4> edge_snap_offsets{};
    std::array<float, 4> edge_snap_scores{};
    std::string edge_snap_error;
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
    std::string detail;
};

std::string describe_candidate(const GeometryCandidate& candidate)
{
    char buffer[768]{};
    std::snprintf(buffer, sizeof(buffer),
        "area=%.5f coverage=%.4f fill=%.4f residual=%.3f inliers=%.3f "
        "snap=%d corners=[%.1f,%.1f;%.1f,%.1f;%.1f,%.1f;%.1f,%.1f] "
        "offsets=[%.1f,%.1f,%.1f,%.1f] scores=[%.1f,%.1f,%.1f,%.1f]",
        candidate.area_ratio,
        candidate.mask_coverage, candidate.quad_fill,
        candidate.edge_residual, candidate.edge_inlier_fraction,
        candidate.edge_snap_applied ? 1 : 0,
        candidate.corners[0].x, candidate.corners[0].y,
        candidate.corners[1].x, candidate.corners[1].y,
        candidate.corners[2].x, candidate.corners[2].y,
        candidate.corners[3].x, candidate.corners[3].y,
        candidate.edge_snap_offsets[0], candidate.edge_snap_offsets[1],
        candidate.edge_snap_offsets[2], candidate.edge_snap_offsets[3],
        candidate.edge_snap_scores[0], candidate.edge_snap_scores[1],
        candidate.edge_snap_scores[2], candidate.edge_snap_scores[3]);
    return std::string(buffer) + " snap_error=" +
        (candidate.edge_snap_error.empty() ? "none" : candidate.edge_snap_error);
}

struct AxisSpan {
    int coordinate = 0;
    int lower = 0;
    int upper = 0;
};

struct RobustLine {
    cv::Vec4f value{};
    float residual = std::numeric_limits<float>::infinity();
    float inlier_fraction = 0.0f;
};

struct GeometryFit {
    std::array<cv::Point2f, 4> corners{};
    cv::Mat component;
    float area_ratio = 0.0f;
    float component_fraction = 0.0f;
    float mask_coverage = 0.0f;
    float quad_fill = 0.0f;
    float edge_residual = std::numeric_limits<float>::infinity();
    float edge_inlier_fraction = 0.0f;
    bool edge_snap_applied = false;
    double edge_snap_ms = 0.0;
    std::array<float, 4> edge_snap_offsets{};
    std::array<float, 4> edge_snap_scores{};
    std::string edge_snap_error;
    std::string error;
};

float percentile(std::vector<float> values, float fraction);
bool intersect_lines(const RobustLine& first, const RobustLine& second,
                     cv::Point2f& output);

float bilinear_sample(const cv::Mat& image, const cv::Point2f& point)
{
    const float x = std::max(0.0f, std::min(point.x,
        static_cast<float>(image.cols - 1)));
    const float y = std::max(0.0f, std::min(point.y,
        static_cast<float>(image.rows - 1)));
    const int x0 = cvFloor(x);
    const int y0 = cvFloor(y);
    const int x1 = std::min(x0 + 1, image.cols - 1);
    const int y1 = std::min(y0 + 1, image.rows - 1);
    const float fx = x - x0;
    const float fy = y - y0;
    const float upper = image.at<float>(y0, x0) * (1.0f - fx) +
                        image.at<float>(y0, x1) * fx;
    const float lower = image.at<float>(y1, x0) * (1.0f - fx) +
                        image.at<float>(y1, x1) * fx;
    return upper * (1.0f - fy) + lower * fy;
}

struct EdgeSnapLine {
    RobustLine line;
    float offset = 0.0f;
    float score = 0.0f;
    float continuity = 0.0f;
};

bool snap_quad_to_image_edges(const cv::Mat& bgr,
                              GeometryFit& geometry)
{
    const auto begin = Clock::now();
    if(bgr.empty() || bgr.type() != CV_8UC3) {
        geometry.edge_snap_error = "invalid_bgr";
        geometry.edge_snap_ms = elapsed_ms(begin);
        return false;
    }

    cv::Mat gray_u8;
    cv::cvtColor(bgr, gray_u8, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray_u8, gray_u8, cv::Size(3, 3), 0.0);
    cv::Mat gray;
    gray_u8.convertTo(gray, CV_32F);
    cv::Mat gradient_x;
    cv::Mat gradient_y;
    cv::Sobel(gray, gradient_x, CV_32F, 1, 0, 3);
    cv::Sobel(gray, gradient_y, CV_32F, 0, 1, 3);

    cv::Point2f center(0.0f, 0.0f);
    for(const auto& corner : geometry.corners) center += corner;
    center *= 0.25f;

    std::array<EdgeSnapLine, 4> snapped_lines{};
    int snapped_count = 0;
    for(std::size_t edge = 0; edge < 4; ++edge) {
        const cv::Point2f first = geometry.corners[edge];
        const cv::Point2f second = geometry.corners[(edge + 1) % 4];
        const cv::Point2f delta = second - first;
        const float length = static_cast<float>(cv::norm(delta));
        if(length < 32.0f) {
            geometry.edge_snap_error = "short_edge_" + std::to_string(edge);
            geometry.edge_snap_ms = elapsed_ms(begin);
            return false;
        }
        cv::Point2f tangent = delta * (1.0f / length);
        /* The false-positive region is on the platform's left.  Use the
         * much more stable opposite side to seed the left-edge direction. */
        if(edge == 3) {
            tangent = cv::Point2f(-snapped_lines[1].line.value[0],
                                  -snapped_lines[1].line.value[1]);
        }
        cv::Point2f inward(-tangent.y, tangent.x);
        const cv::Point2f midpoint = (first + second) * 0.5f;
        if(inward.dot(center - midpoint) < 0.0f) inward *= -1.0f;

        const float trim = contract::kGeometryEdgeSnapTrimFraction;
        const int sample_count = std::max(32, cvRound(length * (1.0f - 2.0f * trim)));
        bool found = false;
        EdgeSnapLine best;
        const int search_radius = edge == 3
            ? contract::kGeometryEdgeSnapSideSearchRadius320
            : contract::kGeometryEdgeSnapSearchRadius320;
        const float minimum_normal_gradient = edge == 3
            ? contract::kGeometryEdgeSnapSideMinNormalGradient
            : contract::kGeometryEdgeSnapMinNormalGradient;
        const float minimum_continuity = edge == 3
            ? contract::kGeometryEdgeSnapSideMinContinuity
            : contract::kGeometryEdgeSnapMinContinuity;
        const float minimum_score = edge == 3
            ? contract::kGeometryEdgeSnapSideMinScore
            : contract::kGeometryEdgeSnapMinScore;
        const float distance_penalty = edge == 3
            ? contract::kGeometryEdgeSnapSideDistancePenalty
            : contract::kGeometryEdgeSnapDistancePenalty;
        for(int integer_offset = -search_radius;
            integer_offset <= search_radius;
            ++integer_offset) {
            const float offset = static_cast<float>(integer_offset);
            std::vector<float> strengths;
            strengths.reserve(sample_count);
            int continuous = 0;
            for(int sample = 0; sample < sample_count; ++sample) {
                const float t = trim + (1.0f - 2.0f * trim) *
                    (static_cast<float>(sample) + 0.5f) / sample_count;
                const cv::Point2f point = midpoint +
                    tangent * ((t - 0.5f) * length) + inward * offset;
                if(point.x < 2.0f || point.x >= bgr.cols - 2.0f ||
                   point.y < 2.0f || point.y >= bgr.rows - 2.0f) {
                    continue;
                }
                const float normal_gradient = std::abs(
                    bilinear_sample(gradient_x, point) * inward.x +
                    bilinear_sample(gradient_y, point) * inward.y);
                const float center_gray = bilinear_sample(gray, point);
                const float neighbor_gray = 0.5f * (
                    bilinear_sample(gray, point - inward * 2.0f) +
                    bilinear_sample(gray, point + inward * 2.0f));
                const float dark_gap = std::max(0.0f, neighbor_gray - center_gray);
                const float strength = normal_gradient + 0.6f * dark_gap;
                strengths.push_back(strength);
                if(normal_gradient >= minimum_normal_gradient) {
                    ++continuous;
                }
            }
            if(strengths.size() < 24) continue;
            const float continuity = static_cast<float>(continuous) /
                                     strengths.size();
            const float robust_strength =
                0.65f * percentile(strengths, 0.50f) +
                0.35f * percentile(strengths, 0.25f);
            const float score = robust_strength -
                std::abs(offset) * distance_penalty;
            if(continuity >= minimum_continuity &&
               score >= minimum_score &&
               (!found || score > best.score)) {
                found = true;
                best.offset = offset;
                best.score = score;
                best.continuity = continuity;
                best.line.value = cv::Vec4f(tangent.x, tangent.y,
                    midpoint.x + inward.x * offset,
                    midpoint.y + inward.y * offset);
            }
        }
        if(!found) {
            best.offset = 0.0f;
            best.score = 0.0f;
            best.continuity = 0.0f;
            best.line.value = cv::Vec4f(tangent.x, tangent.y,
                                        midpoint.x, midpoint.y);
        } else {
            ++snapped_count;
        }
        snapped_lines[edge] = best;
    }
    if(snapped_count != 4) {
        geometry.edge_snap_error = "incomplete_edge_set_count_" +
            std::to_string(snapped_count);
        geometry.edge_snap_ms = elapsed_ms(begin);
        return false;
    }

    std::array<cv::Point2f, 4> snapped{};
    if(!intersect_lines(snapped_lines[3].line, snapped_lines[0].line, snapped[0]) ||
       !intersect_lines(snapped_lines[0].line, snapped_lines[1].line, snapped[1]) ||
       !intersect_lines(snapped_lines[1].line, snapped_lines[2].line, snapped[2]) ||
       !intersect_lines(snapped_lines[2].line, snapped_lines[3].line, snapped[3])) {
        geometry.edge_snap_error = "intersection";
        geometry.edge_snap_ms = elapsed_ms(begin);
        return false;
    }
    float maximum_corner_delta = 0.0f;
    for(std::size_t corner = 0; corner < snapped.size(); ++corner) {
        maximum_corner_delta = std::max(maximum_corner_delta,
            static_cast<float>(cv::norm(
                snapped[corner] - geometry.corners[corner])));
    }
    if(maximum_corner_delta >
       contract::kGeometryEdgeSnapMaxCornerDelta320) {
        char diagnostic[96]{};
        std::snprintf(diagnostic, sizeof(diagnostic),
            "corner_delta_%.2f_max_%.2f", maximum_corner_delta,
            contract::kGeometryEdgeSnapMaxCornerDelta320);
        geometry.edge_snap_error = diagnostic;
        geometry.edge_snap_ms = elapsed_ms(begin);
        return false;
    }
    const std::vector<cv::Point2f> old_polygon(
        geometry.corners.begin(), geometry.corners.end());
    const std::vector<cv::Point2f> new_polygon(snapped.begin(), snapped.end());
    const double old_area = std::abs(cv::contourArea(old_polygon));
    const double new_area = std::abs(cv::contourArea(new_polygon));
    const float area_scale = static_cast<float>(new_area / std::max(1.0, old_area));
    if(!cv::isContourConvex(new_polygon) ||
       area_scale < contract::kGeometryEdgeSnapAreaRatioMin ||
       area_scale > contract::kGeometryEdgeSnapAreaRatioMax ||
       cv::pointPolygonTest(new_polygon, center, false) < 0.0) {
        geometry.edge_snap_error = "snapped_shape";
        geometry.edge_snap_ms = elapsed_ms(begin);
        return false;
    }
    for(const auto& corner : snapped) {
        if(corner.x < -8.0f || corner.x > bgr.cols + 8.0f ||
            corner.y < -8.0f || corner.y > bgr.rows + 8.0f) {
            geometry.edge_snap_error = "snapped_bounds";
            geometry.edge_snap_ms = elapsed_ms(begin);
            return false;
        }
    }

    const float snapped_area_ratio = static_cast<float>(new_area / bgr.total());
    cv::Mat quad = cv::Mat::zeros(geometry.component.size(), CV_8U);
    std::vector<cv::Point> integer_polygon;
    for(const auto& corner : snapped) {
        integer_polygon.emplace_back(cvRound(corner.x), cvRound(corner.y));
    }
    cv::fillConvexPoly(quad, integer_polygon, cv::Scalar(255));
    cv::Mat intersection;
    cv::bitwise_and(geometry.component, quad, intersection);
    const int intersection_area = cv::countNonZero(intersection);
    const float snapped_mask_coverage = static_cast<float>(intersection_area) /
        std::max(1, cv::countNonZero(geometry.component));
    const float snapped_quad_fill = static_cast<float>(intersection_area) /
        std::max(1, cv::countNonZero(quad));
    if(snapped_area_ratio < contract::kAreaRatioMin ||
       snapped_area_ratio > contract::kAreaRatioMax ||
       snapped_mask_coverage < contract::kGeometryMaskCoverageMin ||
       snapped_quad_fill < contract::kGeometryQuadFillMin) {
        char diagnostic[192]{};
        std::snprintf(diagnostic, sizeof(diagnostic),
            "snapped_quality_area=%.4f_coverage=%.4f_fill=%.4f",
            snapped_area_ratio, snapped_mask_coverage, snapped_quad_fill);
        geometry.edge_snap_error = diagnostic;
        geometry.edge_snap_ms = elapsed_ms(begin);
        return false;
    }
    geometry.corners = snapped;
    geometry.area_ratio = snapped_area_ratio;
    geometry.mask_coverage = snapped_mask_coverage;
    geometry.quad_fill = snapped_quad_fill;
    geometry.edge_snap_applied = true;
    geometry.edge_snap_error.clear();
    for(std::size_t edge = 0; edge < 4; ++edge) {
        geometry.edge_snap_offsets[edge] = snapped_lines[edge].offset;
        geometry.edge_snap_scores[edge] = snapped_lines[edge].score;
    }
    geometry.edge_snap_ms = elapsed_ms(begin);
    return true;
}

float point_line_distance(const cv::Point2f& point, const cv::Vec4f& line)
{
    const cv::Point2f direction(line[0], line[1]);
    const cv::Point2f origin(line[2], line[3]);
    return std::abs(direction.x * (point.y - origin.y) -
                    direction.y * (point.x - origin.x));
}

float percentile(std::vector<float> values, float fraction)
{
    if(values.empty()) {
        return std::numeric_limits<float>::infinity();
    }
    const std::size_t index = std::min(
        values.size() - 1,
        static_cast<std::size_t>(fraction * static_cast<float>(values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[index];
}

bool robust_fit_line(const std::vector<cv::Point2f>& points, RobustLine& output)
{
    if(points.size() < 12) {
        return false;
    }
    cv::fitLine(points, output.value, cv::DIST_WELSCH, 0.0, 0.01, 0.01);
    std::vector<cv::Point2f> inliers;
    inliers.reserve(points.size());
    for(const auto& point : points) {
        if(point_line_distance(point, output.value) <=
           contract::kGeometryEdgeInlierDistance320) {
            inliers.push_back(point);
        }
    }
    output.inlier_fraction = static_cast<float>(inliers.size()) /
                             static_cast<float>(points.size());
    if(inliers.size() < 10 || output.inlier_fraction <
       contract::kGeometryEdgeInlierFractionMin) {
        return false;
    }
    cv::fitLine(inliers, output.value, cv::DIST_L2, 0.0, 0.01, 0.01);
    std::vector<float> residuals;
    residuals.reserve(inliers.size());
    for(const auto& point : inliers) {
        residuals.push_back(point_line_distance(point, output.value));
    }
    output.residual = percentile(std::move(residuals), 0.90f);
    return true;
}

bool horizontal_span_containing(const cv::Mat& mask, int y, int anchor_x,
                                AxisSpan& output)
{
    const uint8_t* row = mask.ptr<uint8_t>(y);
    int x = 0;
    while(x < mask.cols) {
        while(x < mask.cols && row[x] == 0) ++x;
        if(x >= mask.cols) break;
        const int start = x;
        while(x + 1 < mask.cols && row[x + 1] != 0) ++x;
        const int end = x;
        if(start <= anchor_x && anchor_x <= end) {
            output = {y, start, end};
            return true;
        }
        ++x;
    }
    return false;
}

bool vertical_span_containing(const cv::Mat& mask, int x, int anchor_y,
                              AxisSpan& output)
{
    int y = 0;
    while(y < mask.rows) {
        while(y < mask.rows && mask.ptr<uint8_t>(y)[x] == 0) ++y;
        if(y >= mask.rows) break;
        const int start = y;
        while(y + 1 < mask.rows && mask.ptr<uint8_t>(y + 1)[x] != 0) ++y;
        const int end = y;
        if(start <= anchor_y && anchor_y <= end) {
            output = {x, start, end};
            return true;
        }
        ++y;
    }
    return false;
}

float line_x_at_y(const RobustLine& line, float y)
{
    if(std::abs(line.value[1]) < 1e-5f) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return line.value[2] + (y - line.value[3]) * line.value[0] / line.value[1];
}

bool intersect_lines(const RobustLine& first, const RobustLine& second,
                     cv::Point2f& output)
{
    const cv::Point2f p(first.value[2], first.value[3]);
    const cv::Point2f r(first.value[0], first.value[1]);
    const cv::Point2f q(second.value[2], second.value[3]);
    const cv::Point2f s(second.value[0], second.value[1]);
    const float cross = r.x * s.y - r.y * s.x;
    if(std::abs(cross) < 1e-5f) {
        return false;
    }
    const cv::Point2f delta = q - p;
    const float scale = (delta.x * s.y - delta.y * s.x) / cross;
    output = p + scale * r;
    return std::isfinite(output.x) && std::isfinite(output.y);
}

GeometryFit fit_platform_geometry(const cv::Mat& raw_mask)
{
    GeometryFit result;
    if(raw_mask.empty() || raw_mask.type() != CV_8U) {
        result.error = "invalid_mask";
        return result;
    }

    cv::Mat mask;
    const int kernel_size = contract::kGeometryCloseKernel;
    const cv::Mat kernel = cv::getStructuringElement(
        cv::MORPH_ELLIPSE, cv::Size(kernel_size, kernel_size));
    cv::morphologyEx(raw_mask, mask, cv::MORPH_CLOSE, kernel);
    const int foreground = cv::countNonZero(mask);
    if(foreground == 0) {
        result.error = "empty_mask";
        return result;
    }

    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroids;
    const int label_count = cv::connectedComponentsWithStats(
        mask, labels, stats, centroids, 8, CV_32S);
    if(label_count <= 1) {
        result.error = "no_component";
        return result;
    }

    const int central_left = cvRound(mask.cols * contract::kGeometryCentralRoiMin);
    const int central_top = cvRound(mask.rows * contract::kGeometryCentralRoiMin);
    const int central_right = cvRound(mask.cols * contract::kGeometryCentralRoiMax);
    const int central_bottom = cvRound(mask.rows * contract::kGeometryCentralRoiMax);
    const cv::Rect central_roi(
        central_left, central_top,
        std::max(1, central_right - central_left),
        std::max(1, central_bottom - central_top));

    int selected_label = 0;
    int selected_central_pixels = 0;
    int selected_area = 0;
    for(int label = 1; label < label_count; ++label) {
        cv::Mat central_match;
        cv::compare(labels(central_roi), label, central_match, cv::CMP_EQ);
        const int central_pixels = cv::countNonZero(central_match);
        const int area = stats.at<int>(label, cv::CC_STAT_AREA);
        if(central_pixels > selected_central_pixels ||
           (central_pixels == selected_central_pixels && area > selected_area)) {
            selected_label = label;
            selected_central_pixels = central_pixels;
            selected_area = area;
        }
    }
    if(selected_label == 0 || selected_central_pixels == 0) {
        result.error = "position_constraint";
        return result;
    }
    cv::compare(labels, selected_label, result.component, cv::CMP_EQ);
    result.component_fraction = static_cast<float>(selected_area) / foreground;

    /*
     * Closing is used for coverage and small-hole recovery, but its expanded
     * pixels must not bridge a narrow gap between the platform and a false
     * positive blob.  Edge samples therefore come only from pixels that were
     * already present in the raw model mask inside the selected central
     * component.
     */
    cv::Mat edge_component;
    cv::bitwise_and(raw_mask, result.component, edge_component);

    cv::Mat distance;
    cv::distanceTransform(edge_component, distance, cv::DIST_L2, 3);
    double anchor_distance = 0.0;
    cv::Point local_anchor;
    cv::minMaxLoc(distance(central_roi), nullptr, &anchor_distance,
                  nullptr, &local_anchor);
    if(anchor_distance < 4.0) {
        result.error = "position_constraint";
        return result;
    }
    const cv::Point anchor(local_anchor.x + central_roi.x,
                           local_anchor.y + central_roi.y);

    std::vector<AxisSpan> rows;
    for(int y = 0; y < edge_component.rows; ++y) {
        AxisSpan span;
        if(horizontal_span_containing(edge_component, y, anchor.x, span)) {
            rows.push_back(span);
        }
    }
    if(rows.size() < 32) {
        result.error = "edge_fit_rows";
        return result;
    }
    const std::size_t row_trim = std::min(
        rows.size() / 3,
        static_cast<std::size_t>(rows.size() *
                                 contract::kGeometryEdgeTrimFraction));
    std::vector<cv::Point2f> left_points;
    std::vector<cv::Point2f> right_points;
    for(std::size_t index = row_trim; index < rows.size() - row_trim; ++index) {
        left_points.emplace_back(static_cast<float>(rows[index].lower),
                                 static_cast<float>(rows[index].coordinate));
        right_points.emplace_back(static_cast<float>(rows[index].upper),
                                  static_cast<float>(rows[index].coordinate));
    }
    RobustLine left;
    RobustLine right;
    const bool left_fit = robust_fit_line(left_points, left);
    const bool right_fit = robust_fit_line(right_points, right);
    if(!left_fit || !right_fit ||
       std::abs(left.value[1]) < std::abs(left.value[0]) * 0.5f ||
       std::abs(right.value[1]) < std::abs(right.value[0]) * 0.5f) {
        result.error = "edge_fit_sides";
        return result;
    }

    const float left_at_anchor = line_x_at_y(left, static_cast<float>(anchor.y));
    const float right_at_anchor = line_x_at_y(right, static_cast<float>(anchor.y));
    if(!std::isfinite(left_at_anchor) || !std::isfinite(right_at_anchor) ||
       left_at_anchor >= anchor.x - 16.0f || right_at_anchor <= anchor.x + 16.0f) {
        result.error = "position_constraint";
        return result;
    }

    std::vector<AxisSpan> columns;
    const int first_x = std::max(0, cvCeil(left_at_anchor));
    const int last_x = std::min(mask.cols - 1, cvFloor(right_at_anchor));
    for(int x = first_x; x <= last_x; ++x) {
        AxisSpan span;
        if(vertical_span_containing(edge_component, x, anchor.y, span)) {
            columns.push_back(span);
        }
    }
    if(columns.size() < 32) {
        result.error = "edge_fit_columns";
        return result;
    }
    const std::size_t column_trim = std::min(
        columns.size() / 3,
        static_cast<std::size_t>(columns.size() *
                                 contract::kGeometryEdgeTrimFraction));
    std::vector<cv::Point2f> top_points;
    std::vector<cv::Point2f> bottom_points;
    for(std::size_t index = column_trim;
        index < columns.size() - column_trim; ++index) {
        top_points.emplace_back(static_cast<float>(columns[index].coordinate),
                                static_cast<float>(columns[index].lower));
        bottom_points.emplace_back(static_cast<float>(columns[index].coordinate),
                                   static_cast<float>(columns[index].upper));
    }
    RobustLine top;
    RobustLine bottom;
    if(!robust_fit_line(top_points, top) ||
       !robust_fit_line(bottom_points, bottom) ||
       std::abs(top.value[0]) < std::abs(top.value[1]) * 0.5f ||
       std::abs(bottom.value[0]) < std::abs(bottom.value[1]) * 0.5f) {
        result.error = "edge_fit_top_bottom";
        return result;
    }

    if(!intersect_lines(top, left, result.corners[0]) ||
       !intersect_lines(top, right, result.corners[1]) ||
       !intersect_lines(bottom, right, result.corners[2]) ||
       !intersect_lines(bottom, left, result.corners[3])) {
        result.error = "edge_fit_intersection";
        return result;
    }

    std::vector<cv::Point2f> polygon(result.corners.begin(), result.corners.end());
    const double quad_area = std::abs(cv::contourArea(polygon));
    if(quad_area <= 1.0 || !cv::isContourConvex(polygon) ||
       cv::pointPolygonTest(polygon, cv::Point2f(anchor), false) < 0.0) {
        result.error = "position_constraint";
        return result;
    }
    const float top_length = static_cast<float>(
        cv::norm(result.corners[1] - result.corners[0]));
    const float right_length = static_cast<float>(
        cv::norm(result.corners[2] - result.corners[1]));
    const float bottom_length = static_cast<float>(
        cv::norm(result.corners[3] - result.corners[2]));
    const float left_length = static_cast<float>(
        cv::norm(result.corners[0] - result.corners[3]));
    const auto length_balance = [](float first, float second) {
        return std::min(first, second) / std::max(1.0f, std::max(first, second));
    };
    if(length_balance(top_length, bottom_length) <
           contract::kGeometryOppositeEdgeLengthRatioMin ||
       length_balance(left_length, right_length) <
           contract::kGeometryOppositeEdgeLengthRatioMin) {
        result.error = "shape_constraint";
        return result;
    }
    cv::Point2f center(0.0f, 0.0f);
    for(const auto& corner : result.corners) {
        if(corner.x < -8.0f || corner.x > mask.cols + 8.0f ||
           corner.y < -8.0f || corner.y > mask.rows + 8.0f) {
            result.error = "position_constraint";
            return result;
        }
        center += corner;
    }
    center *= 0.25f;
    const float center_min = contract::kGeometryCentralRoiMin;
    const float center_max = contract::kGeometryCentralRoiMax;
    if(center.x < mask.cols * center_min || center.x > mask.cols * center_max ||
       center.y < mask.rows * center_min || center.y > mask.rows * center_max) {
        result.error = "position_constraint";
        return result;
    }

    cv::Mat quad = cv::Mat::zeros(mask.size(), CV_8U);
    std::vector<cv::Point> integer_polygon;
    for(const auto& corner : result.corners) {
        integer_polygon.emplace_back(cvRound(corner.x), cvRound(corner.y));
    }
    cv::fillConvexPoly(quad, integer_polygon, cv::Scalar(255));
    cv::Mat intersection;
    cv::bitwise_and(result.component, quad, intersection);
    const int intersection_area = cv::countNonZero(intersection);
    const int quad_pixels = cv::countNonZero(quad);
    result.area_ratio = static_cast<float>(quad_area) / mask.total();
    result.mask_coverage = static_cast<float>(intersection_area) /
                           std::max(1, selected_area);
    result.quad_fill = static_cast<float>(intersection_area) /
                       std::max(1, quad_pixels);
    result.edge_residual = std::max(
        std::max(left.residual, right.residual),
        std::max(top.residual, bottom.residual));
    result.edge_inlier_fraction = std::min(
        std::min(left.inlier_fraction, right.inlier_fraction),
        std::min(top.inlier_fraction, bottom.inlier_fraction));
    return result;
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
                           const cv::Mat& canonical_bare,
                           cv::Mat* reference_change)
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

    cv::Mat reference_change_channel = fixed_signed_u8(
        log_values - bare_log, fusion_model::kReferenceChangeLogLimit);
    std::vector<cv::Mat> channels{
        canonical_gray,
        fixed_signed_u8(log_values - fine_illumination, 0.12f),
        fixed_centered_u8(log_values - low_frequency,
                          fusion_model::kLowFrequencyLogLimit),
        fixed_unsigned_u8(gradient, 0.90f),
        std::move(reference_change_channel),
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
    if(reference_change) {
        *reference_change = resized_channels.back().clone();
    }
    cv::Mat merged;
    cv::merge(resized_channels, merged);
    return merged.isContinuous() ? merged : merged.clone();
}

struct LocalPowderGateResult {
    bool reference_change_passed = false;
    bool coverage_passed = false;
    bool connected_component_passed = false;
    bool passed = false;
    double reference_change_mean_abs = 0.0;
    double coverage = 0.0;
    uint32_t bare_cells = 0;
    uint32_t max_bare_component = 0;
};

double patch_correlation(const cv::Mat& first, const cv::Mat& second,
                         int left, int top, int width, int height)
{
    double sum_first = 0.0;
    double sum_second = 0.0;
    double sum_first_squared = 0.0;
    double sum_second_squared = 0.0;
    double sum_product = 0.0;
    for(int row = top; row < top + height; ++row) {
        const float* first_row = first.ptr<float>(row) + left;
        const float* second_row = second.ptr<float>(row) + left;
        for(int column = 0; column < width; ++column) {
            const double first_value = first_row[column];
            const double second_value = second_row[column];
            sum_first += first_value;
            sum_second += second_value;
            sum_first_squared += first_value * first_value;
            sum_second_squared += second_value * second_value;
            sum_product += first_value * second_value;
        }
    }
    const double count = static_cast<double>(width * height);
    const double first_variance =
        count * sum_first_squared - sum_first * sum_first;
    const double second_variance =
        count * sum_second_squared - sum_second * sum_second;
    const double denominator = std::sqrt(
        std::max(first_variance * second_variance, 0.0));
    if(denominator <= 1.0e-12) {
        return 1.0;
    }
    return std::max(-1.0, std::min(
        1.0, (count * sum_product - sum_first * sum_second) / denominator));
}

LocalPowderGateResult evaluate_local_powder_gate(
    const cv::Mat& canonical_gray, const cv::Mat& canonical_bare,
    const cv::Mat& reference_change)
{
    using namespace local_powder_gate;
    if(canonical_gray.empty() || canonical_bare.empty() ||
       reference_change.empty() || canonical_gray.type() != CV_8UC1 ||
       canonical_bare.type() != CV_8UC1 ||
       reference_change.type() != CV_8UC1 ||
       canonical_gray.size() != canonical_bare.size() ||
       canonical_gray.rows % static_cast<int>(kEvidenceGridSize) != 0 ||
       canonical_gray.cols % static_cast<int>(kEvidenceGridSize) != 0 ||
       reference_change.rows % static_cast<int>(kEvidenceGridSize) != 0 ||
       reference_change.cols % static_cast<int>(kEvidenceGridSize) != 0) {
        throw std::runtime_error("invalid local powder gate input");
    }

    cv::Mat current_log;
    cv::Mat bare_log;
    canonical_gray.convertTo(current_log, CV_32F);
    canonical_bare.convertTo(bare_log, CV_32F);
    current_log += fusion_model::kLogFloor;
    bare_log += fusion_model::kLogFloor;
    cv::log(current_log, current_log);
    cv::log(bare_log, bare_log);
    cv::Mat current_fine;
    cv::Mat bare_fine;
    cv::GaussianBlur(current_log, current_fine, cv::Size(), 2.0, 2.0,
                     cv::BORDER_REFLECT_101);
    cv::GaussianBlur(bare_log, bare_fine, cv::Size(), 2.0, 2.0,
                     cv::BORDER_REFLECT_101);
    current_fine = current_log - current_fine;
    bare_fine = bare_log - bare_fine;

    constexpr std::size_t kEvidenceCellCount =
        kEvidenceGridSize * kEvidenceGridSize;
    constexpr std::size_t kDecisionCellCount =
        kDecisionGridSize * kDecisionGridSize;
    std::array<double, kEvidenceCellCount> correlations{};
    std::array<double, kEvidenceCellCount> signed_changes{};
    const int canonical_cell_width =
        canonical_gray.cols / static_cast<int>(kEvidenceGridSize);
    const int canonical_cell_height =
        canonical_gray.rows / static_cast<int>(kEvidenceGridSize);
    const int change_cell_width =
        reference_change.cols / static_cast<int>(kEvidenceGridSize);
    const int change_cell_height =
        reference_change.rows / static_cast<int>(kEvidenceGridSize);

    double total_absolute_change = 0.0;
    for(int row = 0; row < reference_change.rows; ++row) {
        const uint8_t* values = reference_change.ptr<uint8_t>(row);
        for(int column = 0; column < reference_change.cols; ++column) {
            total_absolute_change += std::abs(
                static_cast<int>(values[column]) - 128);
        }
    }

    for(uint32_t grid_row = 0; grid_row < kEvidenceGridSize; ++grid_row) {
        for(uint32_t grid_column = 0; grid_column < kEvidenceGridSize;
            ++grid_column) {
            const std::size_t index =
                static_cast<std::size_t>(grid_row) * kEvidenceGridSize +
                grid_column;
            correlations[index] = patch_correlation(
                bare_fine, current_fine,
                static_cast<int>(grid_column) * canonical_cell_width,
                static_cast<int>(grid_row) * canonical_cell_height,
                canonical_cell_width, canonical_cell_height);
            double signed_sum = 0.0;
            for(int row = static_cast<int>(grid_row) * change_cell_height;
                row < static_cast<int>(grid_row + 1) * change_cell_height;
                ++row) {
                const uint8_t* values = reference_change.ptr<uint8_t>(row);
                for(int column =
                        static_cast<int>(grid_column) * change_cell_width;
                    column < static_cast<int>(grid_column + 1) *
                        change_cell_width;
                    ++column) {
                    signed_sum += static_cast<int>(values[column]) - 128;
                }
            }
            signed_changes[index] = signed_sum /
                static_cast<double>(change_cell_width * change_cell_height);
        }
    }

    std::array<bool, kDecisionCellCount> bare_cells{};
    for(uint32_t decision_row = 0; decision_row < kDecisionGridSize;
        ++decision_row) {
        for(uint32_t decision_column = 0;
            decision_column < kDecisionGridSize; ++decision_column) {
            double correlation = 0.0;
            double signed_change = 0.0;
            for(uint32_t offset_row = 0; offset_row < 2; ++offset_row) {
                for(uint32_t offset_column = 0; offset_column < 2;
                    ++offset_column) {
                    const uint32_t evidence_row = decision_row * 2 + offset_row;
                    const uint32_t evidence_column =
                        decision_column * 2 + offset_column;
                    const std::size_t evidence_index =
                        static_cast<std::size_t>(evidence_row) *
                        kEvidenceGridSize + evidence_column;
                    correlation += correlations[evidence_index];
                    signed_change += signed_changes[evidence_index];
                }
            }
            correlation *= 0.25;
            signed_change *= 0.25;
            const std::size_t decision_index =
                static_cast<std::size_t>(decision_row) * kDecisionGridSize +
                decision_column;
            bare_cells[decision_index] =
                correlation >= kBareFineTextureCorrelation ||
                signed_change < kMinPowderSignedChange;
        }
    }

    LocalPowderGateResult result;
    result.reference_change_mean_abs = total_absolute_change /
        static_cast<double>(reference_change.total());
    for(uint32_t row = kDecisionBorder;
        row < kDecisionGridSize - kDecisionBorder; ++row) {
        for(uint32_t column = kDecisionBorder;
            column < kDecisionGridSize - kDecisionBorder; ++column) {
            const std::size_t index =
                static_cast<std::size_t>(row) * kDecisionGridSize + column;
            if(bare_cells[index]) {
                ++result.bare_cells;
            }
        }
    }
    const uint32_t covered_cells =
        kEvaluatedDecisionCells - result.bare_cells;
    result.coverage = static_cast<double>(covered_cells) /
        kEvaluatedDecisionCells;

    std::array<bool, kDecisionCellCount> visited{};
    std::array<std::size_t, kDecisionCellCount> queue{};
    for(uint32_t row = kDecisionBorder;
        row < kDecisionGridSize - kDecisionBorder; ++row) {
        for(uint32_t column = kDecisionBorder;
            column < kDecisionGridSize - kDecisionBorder; ++column) {
            const std::size_t start =
                static_cast<std::size_t>(row) * kDecisionGridSize + column;
            if(!bare_cells[start] || visited[start]) {
                continue;
            }
            std::size_t head = 0;
            std::size_t tail = 0;
            queue[tail++] = start;
            visited[start] = true;
            uint32_t component_size = 0;
            while(head < tail) {
                const std::size_t current = queue[head++];
                ++component_size;
                const int current_row =
                    static_cast<int>(current / kDecisionGridSize);
                const int current_column =
                    static_cast<int>(current % kDecisionGridSize);
                constexpr std::array<int, 4> kRowOffsets{1, -1, 0, 0};
                constexpr std::array<int, 4> kColumnOffsets{0, 0, 1, -1};
                for(std::size_t direction = 0; direction < 4; ++direction) {
                    const int next_row = current_row + kRowOffsets[direction];
                    const int next_column =
                        current_column + kColumnOffsets[direction];
                    if(next_row < static_cast<int>(kDecisionBorder) ||
                       next_row >= static_cast<int>(
                           kDecisionGridSize - kDecisionBorder) ||
                       next_column < static_cast<int>(kDecisionBorder) ||
                       next_column >= static_cast<int>(
                           kDecisionGridSize - kDecisionBorder)) {
                        continue;
                    }
                    const std::size_t next =
                        static_cast<std::size_t>(next_row) *
                        kDecisionGridSize + static_cast<std::size_t>(next_column);
                    if(bare_cells[next] && !visited[next]) {
                        visited[next] = true;
                        queue[tail++] = next;
                    }
                }
            }
            result.max_bare_component = std::max(
                result.max_bare_component, component_size);
        }
    }

    result.reference_change_passed =
        result.reference_change_mean_abs >= kMinReferenceChangeMeanAbs;
    result.coverage_passed =
        covered_cells >= kMinCoveredDecisionCells;
    result.connected_component_passed =
        result.max_bare_component <= kMaxConnectedBareDecisionCells;
    result.passed = result.reference_change_passed && result.coverage_passed &&
        result.connected_component_passed;
    return result;
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
            output.detail = observation.detail;
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
                                uint32_t frame_seq, std::string& reason,
                                BareReferenceDiagnostic* diagnostic)
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
            if(diagnostic) {
                const cv::Mat canonical_tight = canonical.isContinuous()
                    ? canonical : canonical.clone();
                diagnostic->canonical_width =
                    static_cast<uint32_t>(canonical_tight.cols);
                diagnostic->canonical_height =
                    static_cast<uint32_t>(canonical_tight.rows);
                diagnostic->canonical_gray.assign(
                    canonical_tight.ptr<uint8_t>(),
                    canonical_tight.ptr<uint8_t>() + canonical_tight.total());
            }
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
            cv::Mat reference_change;
            cv::Mat fusion_input = build_fusion_input(
                canonical, canonical_bare, &reference_change);
            const LocalPowderGateResult local_powder =
                evaluate_local_powder_gate(
                    canonical, canonical_bare, reference_change);
            result.local_powder_gate_passed = local_powder.passed;
            result.reference_change_mean_abs =
                local_powder.reference_change_mean_abs;
            result.local_powder_coverage = local_powder.coverage;
            result.local_bare_cells = local_powder.bare_cells;
            result.local_bare_max_component =
                local_powder.max_bare_component;
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
                if(result.output_class == "ready_for_first_layer" &&
                   !local_powder.passed) {
                    result.output_class = "not_ready_for_first_layer";
                    if(!local_powder.reference_change_passed) {
                        reasons.emplace_back("reference_change_too_small");
                    }
                    if(!local_powder.coverage_passed) {
                        reasons.emplace_back(
                            "local_powder_coverage_below_threshold");
                    }
                    if(!local_powder.connected_component_passed) {
                        reasons.emplace_back(
                            "local_bare_component_too_large");
                    }
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
                const cv::Mat bare_tight = canonical_bare.isContinuous()
                    ? canonical_bare : canonical_bare.clone();
                result.canonical_bare_gray.assign(
                    bare_tight.ptr<uint8_t>(),
                    bare_tight.ptr<uint8_t>() + bare_tight.total());
                const cv::Mat reference_change_tight = reference_change.isContinuous()
                    ? reference_change : reference_change.clone();
                result.reference_change_width =
                    static_cast<uint32_t>(reference_change_tight.cols);
                result.reference_change_height =
                    static_cast<uint32_t>(reference_change_tight.rows);
                result.reference_change_gray.assign(
                    reference_change_tight.ptr<uint8_t>(),
                    reference_change_tight.ptr<uint8_t>() +
                        reference_change_tight.total());
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
            /*
             * Startup geometry is sampled from a live camera stream.  A
             * transient mask defect must not discard the valid observations
             * already collected in this same session; group scatter still
             * rejects observations that do not describe one stable platform.
             * Cache audits remain conservative and restart their proposal
             * after any failed sample.
             */
            if(cache_available_) {
                candidates_.clear();
            }
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
        result.detail = describe_candidate(candidate);
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
        GeometryFit geometry = fit_platform_geometry(mask);
        if(!geometry.error.empty()) {
            reason = geometry.error;
            return false;
        }
        snap_quad_to_image_edges(resized, geometry);
        const float mean_probability = static_cast<float>(
            cv::mean(probability, geometry.component)[0]);
        std::vector<std::string> failures;
        if(geometry.area_ratio < contract::kAreaRatioMin ||
           geometry.area_ratio > contract::kAreaRatioMax)
            failures.emplace_back("area_ratio");
        if(cache_available_ &&
           std::abs(geometry.area_ratio / cache_.area_ratio - 1.0f) >
               contract::kCachedAreaRelativeTolerance)
            failures.emplace_back("cached_area_delta");
        if(geometry.component_fraction < contract::kComponentFractionMin)
            failures.emplace_back("fragmented");
        if(geometry.mask_coverage < contract::kGeometryMaskCoverageMin)
            failures.emplace_back("mask_coverage");
        if(geometry.quad_fill < contract::kGeometryQuadFillMin)
            failures.emplace_back("quad_fill");
        /*
         * A successful image-edge snap has already required all four sides
         * to satisfy the original-image gradient/continuity gates.  The mask
         * line residual describes only the coarse seed in that case and must
         * not veto a stronger four-side image observation.
         */
        if(!geometry.edge_snap_applied) {
            if(geometry.edge_inlier_fraction <
               contract::kGeometryEdgeInlierFractionMin)
                failures.emplace_back("edge_support");
            if(geometry.edge_residual > contract::kGeometryEdgeResidualMax320)
                failures.emplace_back("edge_residual");
        }
        if(mean_probability < contract::kMeanProbabilityMin)
            failures.emplace_back("low_probability");
        if(!failures.empty()) {
            reason = join_reasons(failures);
            return false;
        }
        candidate.corners = geometry.corners;
        candidate.area_ratio = geometry.area_ratio;
        candidate.component_fraction = geometry.component_fraction;
        candidate.mask_coverage = geometry.mask_coverage;
        candidate.quad_fill = geometry.quad_fill;
        candidate.edge_residual = geometry.edge_residual;
        candidate.edge_inlier_fraction = geometry.edge_inlier_fraction;
        candidate.mean_probability = mean_probability;
        candidate.edge_snap_applied = geometry.edge_snap_applied;
        candidate.edge_snap_offsets = geometry.edge_snap_offsets;
        candidate.edge_snap_scores = geometry.edge_snap_scores;
        candidate.edge_snap_error = geometry.edge_snap_error;
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
            std::vector<GeometryCandidate> best_cluster;
            for(const auto& seed : candidates_) {
                std::vector<GeometryCandidate> cluster;
                for(const auto& sample : candidates_) {
                    float maximum = 0.0f;
                    for(std::size_t point = 0; point < 4; ++point) {
                        maximum = std::max(maximum, static_cast<float>(
                            cv::norm(sample.corners[point] - seed.corners[point])));
                    }
                    if(maximum <= contract::kGroupScatterMax320) {
                        cluster.push_back(sample);
                    }
                }
                if(cluster.size() > best_cluster.size()) {
                    best_cluster = std::move(cluster);
                }
            }
            if(static_cast<int>(best_cluster.size()) >= required) {
                candidates_ = std::move(best_cluster);
                summary = summarize_group(candidates_);
            } else {
                const std::size_t maximum_window =
                    static_cast<std::size_t>(required * 4);
                if(candidates_.size() > maximum_window) {
                    candidates_.erase(candidates_.begin(),
                        candidates_.begin() + (candidates_.size() - maximum_window));
                }
                next_segmentation_frame_ = frame_seq + 1;
                return "unstable_candidate_group";
            }
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
    uint32_t frame_seq, std::string& reason,
    BareReferenceDiagnostic* diagnostic)
{
    return impl_->capture_bare_reference(
        nv12, bytes, width, height, frame_seq, reason, diagnostic);
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
