/**
 * 激光光斑查表测高 — 算法实现 (C++ / OpenCV)
 *
 * 本文件实现算法 + 内置轻量 JSON 解析器 (无第三方依赖，与 frame_diff_pipeline
 * 保持一致的自包含风格)。公共 API 见 plate_height_pipeline.hpp。
 *
 * 移植自 LaserSpotDetection/src/methods.cpp:
 *   - seg::fixedThresholdSegment -> FixedThresholdSegmenter::segment
 *   - seg::loadHeightLut          -> HeightLut::load
 *   - seg::computeHeightFromZ     -> HeightLut::lookup
 *
 * 设计要点:
 *   - 分割 / 预处理为策略接口，可替换可扩展，无需改本文件
 *   - 配置全部从 plate_height_config.json 读取，调参不重编译
 *   - 不写文件、不打印日志 (异常用 throw 抛出)，与 frame_diff_pipeline 风格一致
 */

#include "plate_height_pipeline.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace upc::visual {

// ================================================================
//  极简 JSON 解析器 (无第三方依赖，仅支持配置文件所需子集)
//  与 frame_diff_pipeline.cpp 内置解析器保持一致风格。
// ================================================================
namespace json {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Var = std::variant<std::monostate, bool, double, std::string, Array, Object>;

    Value() = default;
    explicit Value(Var v) : v_(std::move(v)) {}

    Type type() const { return static_cast<Type>(v_.index()); }
    bool is_null() const { return type() == Type::Null; }
    bool is_bool() const { return type() == Type::Bool; }
    bool is_number() const { return type() == Type::Number; }
    bool is_string() const { return type() == Type::String; }
    bool is_array() const { return type() == Type::Array; }
    bool is_object() const { return type() == Type::Object; }

    bool as_bool() const { return std::get<bool>(v_); }
    double as_number() const { return std::get<double>(v_); }
    const std::string& as_string() const { return std::get<std::string>(v_); }
    const Array& as_array() const { return std::get<Array>(v_); }
    const Object& as_object() const { return std::get<Object>(v_); }

    const Value* find(const std::string& key) const {
        if (!is_object()) return nullptr;
        auto it = as_object().find(key);
        return it == as_object().end() ? nullptr : &it->second;
    }

private:
    Var v_;
};

class Parser {
public:
    explicit Parser(const std::string& text)
        : p_(text.data()), end_(text.data() + text.size()) {}

    Value parse() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (p_ != end_) throw std::runtime_error("JSON 末尾有残余字符");
        return v;
    }

private:
    const char* p_;
    const char* end_;

    void skip_ws() {
        while (p_ < end_ && std::isspace(static_cast<unsigned char>(*p_))) ++p_;
    }
    char peek() {
        if (p_ >= end_) throw std::runtime_error("JSON 意外结束");
        return *p_;
    }
    char next() {
        if (p_ >= end_) throw std::runtime_error("JSON 意外结束");
        return *p_++;
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        switch (c) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': return Value(std::move(parse_string()));
            case 't': case 'f': return Value(parse_bool());
            case 'n': return parse_null();
            default: return Value(parse_number());
        }
    }

    Value parse_object() {
        next();  // '{'
        Object obj;
        skip_ws();
        if (peek() == '}') { next(); return Value(std::move(obj)); }
        for (;;) {
            skip_ws();
            if (peek() != '"') throw std::runtime_error("JSON 对象键必须是字符串");
            std::string key = parse_string();
            skip_ws();
            if (next() != ':') throw std::runtime_error("JSON 缺少冒号");
            obj.emplace(std::move(key), parse_value());
            skip_ws();
            char c = next();
            if (c == ',') continue;
            if (c == '}') break;
            throw std::runtime_error("JSON 对象缺少逗号或右花括号");
        }
        return Value(std::move(obj));
    }

    Value parse_array() {
        next();  // '['
        Array arr;
        skip_ws();
        if (peek() == ']') { next(); return Value(std::move(arr)); }
        for (;;) {
            arr.push_back(parse_value());
            skip_ws();
            char c = next();
            if (c == ',') continue;
            if (c == ']') break;
            throw std::runtime_error("JSON 数组缺少逗号或右方括号");
        }
        return Value(std::move(arr));
    }

    std::string parse_string() {
        next();  // '"'
        std::string s;
        while (p_ < end_) {
            char c = next();
            if (c == '"') return s;
            if (c == '\\') {
                char e = next();
                switch (e) {
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/': s.push_back('/'); break;
                    case 'b': s.push_back('\b'); break;
                    case 'f': s.push_back('\f'); break;
                    case 'n': s.push_back('\n'); break;
                    case 'r': s.push_back('\r'); break;
                    case 't': s.push_back('\t'); break;
                    case 'u':
                        for (int i = 0; i < 4 && p_ < end_; ++i) ++p_;
                        s.push_back('?');
                        break;
                    default: throw std::runtime_error("JSON 非法转义");
                }
            } else {
                s.push_back(c);
            }
        }
        throw std::runtime_error("JSON 字符串未闭合");
    }

    bool parse_bool() {
        if (end_ - p_ >= 4 && std::strncmp(p_, "true", 4) == 0) { p_ += 4; return true; }
        if (end_ - p_ >= 5 && std::strncmp(p_, "false", 5) == 0) { p_ += 5; return false; }
        throw std::runtime_error("JSON 非法布尔值");
    }

    Value parse_null() {
        if (end_ - p_ >= 4 && std::strncmp(p_, "null", 4) == 0) { p_ += 4; return Value{}; }
        throw std::runtime_error("JSON 非法 null");
    }

    double parse_number() {
        const char* start = p_;
        if (peek() == '-') ++p_;
        while (p_ < end_ && (std::isdigit(static_cast<unsigned char>(*p_)) ||
                            *p_ == '.' || *p_ == 'e' || *p_ == 'E' ||
                            *p_ == '+' || *p_ == '-')) {
            ++p_;
        }
        std::string num(start, p_ - start);
        try {
            size_t idx = 0;
            double v = std::stod(num, &idx);
            if (idx != num.size()) throw std::runtime_error("JSON 数字解析不完整");
            return v;
        } catch (const std::exception&) {
            throw std::runtime_error("JSON 非法数字: " + num);
        }
    }
};

inline Value parse_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("无法打开配置文件: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return Parser(ss.str()).parse();
}

template <typename T>
T get(const Value& root, const std::string& key, const T& fallback);

template <>
int get<int>(const Value& root, const std::string& key, const int& fallback) {
    const Value* v = root.find(key);
    if (!v || !v->is_number()) return fallback;
    return static_cast<int>(v->as_number());
}

template <>
double get<double>(const Value& root, const std::string& key, const double& fallback) {
    const Value* v = root.find(key);
    if (!v || !v->is_number()) return fallback;
    return v->as_number();
}

template <>
std::string get<std::string>(const Value& root, const std::string& key,
                              const std::string& fallback) {
    const Value* v = root.find(key);
    if (!v || !v->is_string()) return fallback;
    return v->as_string();
}

template <>
bool get<bool>(const Value& root, const std::string& key, const bool& fallback) {
    const Value* v = root.find(key);
    if (!v || !v->is_bool()) return fallback;
    return v->as_bool();
}

/** 解析不规则四边形 ROI: [[x1,y1],[x2,y2],[x3,y3],[x4,y4]]，4 个顶点；
 *  缺失/格式错/不是 4 个点返回空 vector。 */
inline std::vector<cv::Point> get_quad(const Value& root, const std::string& key) {
    std::vector<cv::Point> quad;
    const Value* v = root.find(key);
    if (!v || !v->is_array() || v->as_array().size() != 4) {
        return quad;
    }
    const auto& a = v->as_array();
    quad.reserve(4);
    for (int i = 0; i < 4; ++i) {
        if (!a[i].is_array() || a[i].as_array().size() < 2) {
            return {};
        }
        const auto& p = a[i].as_array();
        if (!p[0].is_number() || !p[1].is_number()) {
            return {};
        }
        quad.emplace_back(static_cast<int>(p[0].as_number()),
                           static_cast<int>(p[1].as_number()));
    }
    return quad;
}

}  // namespace json

// ================================================================
//  配置加载
// ================================================================
PlateHeightConfig load_plate_height_config(const std::string& config_path) {
    const json::Value root = json::parse_file(config_path);

    PlateHeightConfig cfg;
    cfg.roi_quad     = json::get_quad(root, "roi_quad");
    cfg.fixed_thresh = static_cast<uint8_t>(
        json::get<int>(root, "fixed_thresh", cfg.fixed_thresh));
    cfg.min_area     = json::get<int>(root, "min_area",     cfg.min_area);
    cfg.max_area     = json::get<int>(root, "max_area",     cfg.max_area);
    cfg.lut_path     = json::get<std::string>(root, "lut_path", cfg.lut_path);
    cfg.enable_clahe = json::get<bool>(root, "enable_clahe", cfg.enable_clahe);
    cfg.clahe_clip   = json::get<double>(root, "clahe_clip", cfg.clahe_clip);
    cfg.enable_blur  = json::get<bool>(root, "enable_blur",  cfg.enable_blur);
    cfg.blur_k       = json::get<int>(root, "blur_k",       cfg.blur_k);
    return cfg;
}

// ================================================================
//  预处理策略
// ================================================================
cv::Mat NullPreProcessor::process(const cv::Mat& gray, const PlateHeightConfig& cfg) const {
    (void)cfg;
    return gray;  // 直通
}

cv::Mat ClahePreProcessor::process(const cv::Mat& gray, const PlateHeightConfig& cfg) const {
    cv::Mat out;
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(cfg.clahe_clip, cv::Size(8, 8));
    clahe->apply(gray, out);
    return out;
}

// ================================================================
//  分割策略: 固定阈值法 (移植自 seg::fixedThresholdSegment)
// ================================================================
LaserSpot FixedThresholdSegmenter::segment(const cv::Mat& src,
                                           const PlateHeightConfig& cfg) const {
    LaserSpot res;
    res.valid = false;

    // 统一转灰度
    cv::Mat gray;
    if (src.type() == CV_8UC3) {
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
    } else if (src.type() == CV_8UC1) {
        gray = src;
    } else {
        return res;
    }

    // 全图尺寸 mask，后面只贴选中的光斑
    res.mask = cv::Mat::zeros(gray.size(), CV_8UC1);

    // ROI 处理：不规则四边形(4 个顶点)，用多边形 mask 过滤 bbox 内不在四边形里的像素；
    // roi_quad 为空表示整幅图。
    const cv::Rect imageRect(0, 0, gray.cols, gray.rows);
    cv::Rect roiBox;          // 实际处理区域 bounding box (原图坐标)
    cv::Mat roiShapeMask;     // bbox 内的形状 mask(255=ROI 内)；空表示整图不限制

    if (!cfg.roi_quad.empty()) {
        // 不规则四边形：4 个顶点 → bbox 求交 → 局部坐标 fillConvexPoly 生成 mask
        roiBox = cv::boundingRect(cfg.roi_quad) & imageRect;
        if (roiBox.empty()) {
            return res;
        }
        std::vector<cv::Point> localPts;
        localPts.reserve(cfg.roi_quad.size());
        for (const auto& p : cfg.roi_quad) {
            localPts.emplace_back(p.x - roiBox.x, p.y - roiBox.y);
        }
        roiShapeMask = cv::Mat::zeros(roiBox.size(), CV_8UC1);
        cv::fillConvexPoly(roiShapeMask, localPts, cv::Scalar(255));
    } else {
        // 未配 roi_quad：整幅图
        roiBox = imageRect;
    }

    cv::Mat roiGray = gray(roiBox);
    cv::Mat roiBin;
    cv::threshold(roiGray, roiBin, cfg.fixed_thresh, 255, cv::THRESH_BINARY);
    // 四边形：用形状 mask 过滤掉 bbox 内但不在四边形里的像素
    if (!roiShapeMask.empty()) {
        cv::bitwise_and(roiBin, roiShapeMask, roiBin);
    }

    cv::Mat labels, stats, centroids;
    const int numLabels = cv::connectedComponentsWithStats(
        roiBin, labels, stats, centroids, 8, CV_32S);

    // 取面积最大且落在 [min_area, max_area] 区间的连通域
    int bestLabel = -1;
    int bestArea = -1;
    for (int i = 1; i < numLabels; ++i) {
        const int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area >= cfg.min_area && area <= cfg.max_area && area > bestArea) {
            bestArea = area;
            bestLabel = i;
        }
    }
    if (bestLabel < 0) {
        return res;
    }

    // 把选中光斑贴回全图 mask
    cv::Mat roiSpot = cv::Mat::zeros(roiBin.size(), CV_8UC1);
    roiSpot.setTo(255, labels == bestLabel);
    roiSpot.copyTo(res.mask(roiBox));

    // 灰度加权质心 (坐标落在原图坐标系)
    double sumI = 0.0, sumX = 0.0, sumY = 0.0;
    for (int y = roiBox.y; y < roiBox.y + roiBox.height; ++y) {
        const uint8_t* maskRow = res.mask.ptr<uint8_t>(y);
        const uint8_t* grayRow = gray.ptr<uint8_t>(y);
        for (int x = roiBox.x; x < roiBox.x + roiBox.width; ++x) {
            if (maskRow[x] != 255) continue;
            const double g = static_cast<double>(grayRow[x]);
            sumI += g;
            sumX += static_cast<double>(x) * g;
            sumY += static_cast<double>(y) * g;
        }
    }
    if (sumI < 1e-6) {
        return res;
    }

    res.valid = true;
    res.center_x = static_cast<float>(sumX / sumI);
    res.center_y = static_cast<float>(sumY / sumI);
    return res;
}

// ================================================================
//  LUT: 加载 (移植自 seg::loadHeightLut)
// ================================================================
namespace {

std::string trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

}  // namespace

bool HeightLut::load(const std::string& path) {
    tables_.clear();
    zero_index_ = -1;
    y_descending_ = true;

    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        const std::string str = trim(line);
        if (str.empty() || str.front() == '#') {
            continue;
        }
        std::istringstream iss(str);
        float height, x, y;
        if (!(iss >> height >> x >> y)) {
            continue;
        }
        HeightSample item;
        item.height  = height;
        item.laser_x  = x;
        item.laser_y  = y;
        tables_.push_back(item);
    }

    if (tables_.empty()) {
        return false;
    }

    // 按 height(Z) 升序排序
    std::sort(tables_.begin(), tables_.end(),
              [](const HeightSample& a, const HeightSample& b) {
                  return a.height < b.height;
              });

    // 查找基准零点 Z~=0，阈值 0.001
    for (std::size_t i = 0; i < tables_.size(); ++i) {
        if (std::fabs(tables_[i].height) < 1e-3f) {
            zero_index_ = static_cast<int>(i);
            break;
        }
    }

    // 校验 laser_y 是否随 height 升序而递减
    y_descending_ = true;
    for (std::size_t i = 1; i < tables_.size(); ++i) {
        if (tables_[i].laser_y >= tables_[i - 1].laser_y) {
            y_descending_ = false;
            break;
        }
    }
    return true;
}

// ================================================================
//  LUT: 保存标定表 (3列文本：height laser_x laser_y)
// ================================================================
bool HeightLut::save(const std::string& path,
                     const std::vector<HeightSample>& samples)
{
    if (samples.empty()) {
        return false;
    }

    /* 按 height 升序排序副本，保证文件内有序、与 load() 一致 */
    std::vector<HeightSample> sorted = samples;
    std::sort(sorted.begin(), sorted.end(),
              [](const HeightSample& a, const HeightSample& b) {
                  return a.height < b.height;
              });

    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }

    /* 文件头注释（与现有 height_lut.txt 格式一致） */
    file << "# Calibration Points: height laser_x laser_y\n";
    file << "# auto generated by height calibration tool\n";

    /* 写入每行：height  laser_x  laser_y
     * 用默认浮点精度（6位有效数字），足够像素坐标精度。 */
    file << std::fixed;
    file.precision(3);
    for (const auto& s : sorted) {
        file << s.height << "\t" << s.laser_x << "\t" << s.laser_y << "\n";
    }

    return file.good();
}

// ================================================================
//  LUT: 查表插值 (移植自 seg::computeHeightFromZ)
// ================================================================
float HeightLut::lookup(float laser_y) const {
    if (tables_.empty()) {
        return std::nanf("");
    }

    const float yMin = tables_.back().laser_y;  // 最大高度对应的 y (最小 y)
    const float yMax = tables_.front().laser_y;  // Z=0 附近，最大 y 像素

    // y 必须处于 [yMin, yMax]
    if (laser_y < yMin || laser_y > yMax) {
        return std::nanf("");
    }

    // 命中上下边界直接返回
    if (laser_y == yMax) return tables_.front().height;
    if (laser_y == yMin) return tables_.back().height;

    // 找: tables[i-1].laser_y >= laser_y > tables[i].laser_y
    int index = -1;
    for (std::size_t i = 1; i < tables_.size(); ++i) {
        if (laser_y > tables_[i].laser_y) {
            index = static_cast<int>(i);
            break;
        }
    }
    if (index <= 0) {
        return std::nanf("");
    }

    const HeightSample& p0 = tables_[index - 1];
    const HeightSample& p1 = tables_[index];
    const float t = (laser_y - p0.laser_y) / (p1.laser_y - p0.laser_y);
    return p0.height + t * (p1.height - p0.height);
}

// ================================================================
//  顶层入口
// ================================================================
float measure_height(const cv::Mat& gray,
                     const PlateHeightConfig& cfg,
                     const IPreProcessor& pre,
                     const ISpotSegmenter& seg) {
    // 预处理
    cv::Mat prepped = pre.process(gray, cfg);
    if (prepped.empty()) {
        return std::nanf("");
    }

    // 分割
    const LaserSpot spot = seg.segment(prepped, cfg);
    if (!spot.valid) {
        return std::nanf("");
    }

    // 查表
    HeightLut lut;
    if (cfg.lut_path.empty() || !lut.load(cfg.lut_path) || !lut.valid()) {
        return std::nanf("");
    }
    return lut.lookup(spot.center_y);
}

float measure_height(const cv::Mat& gray, const std::string& config_path) {
    const PlateHeightConfig cfg = load_plate_height_config(config_path);
    NullPreProcessor pre;
    FixedThresholdSegmenter seg;
    return measure_height(gray, cfg, pre, seg);
}

LaserSpot detect_laser_spot(const cv::Mat& gray, const std::string& config_path)
{
    const PlateHeightConfig cfg = load_plate_height_config(config_path);
    NullPreProcessor pre;
    FixedThresholdSegmenter seg;
    const cv::Mat processed = pre.process(gray, cfg);
    return seg.segment(processed, cfg);
}

}  // namespace upc::visual
