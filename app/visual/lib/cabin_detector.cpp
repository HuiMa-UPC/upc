#include "cabin_detector.hpp"
#include "cabin_bundled_profile.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace upc::visual {
namespace {
using Box = std::array<double, 4>;
constexpr size_t kMaxPixels = 4096u * 4096u;
bool valid_nv12(const uint8_t* p, size_t bytes, uint32_t w, uint32_t h) {
    return p && w >= 160 && h >= 120 && w <= 8192 && h <= 8192 &&
           static_cast<size_t>(w) * h <= kMaxPixels &&
           !(w & 1) && !(h & 1) && bytes == static_cast<size_t>(w) * h * 3 / 2;
}
double clip(double x) { return std::clamp(x, 0.0, 1.0); }
double percentile_hist(const std::array<size_t, 256>& hist, size_t count, double percent) {
    if(count == 0) throw std::runtime_error("empty image histogram");
    const double index = (count - 1) * percent / 100.0;
    const size_t low = static_cast<size_t>(index), high = static_cast<size_t>(std::ceil(index));
    size_t sum = 0; int a = -1, b = 255;
    for(int i = 0; i < 256; ++i) {
        sum += hist[i];
        if(a < 0 && sum > low) a = i;
        if(sum > high) { b = i; break; }
    }
    return a + (b - a) * (index - low);
}
double percentile(const cv::Mat& image, double percent) {
    std::array<size_t, 256> hist{};
    for(int y = 0; y < image.rows; ++y)
        for(int x = 0; x < image.cols; ++x) ++hist[image.ptr<uint8_t>(y)[x]];
    return percentile_hist(hist,image.total(),percent);
}
cv::Rect rect(const Box& b, int w, int h) {
    const int l = std::clamp(static_cast<int>(std::nearbyint(b[0] * w)), 0, w - 1);
    const int t = std::clamp(static_cast<int>(std::nearbyint(b[1] * h)), 0, h - 1);
    const int r = std::clamp(static_cast<int>(std::nearbyint(b[2] * w)), l + 1, w);
    const int d = std::clamp(static_cast<int>(std::nearbyint(b[3] * h)), t + 1, h);
    return {l, t, r - l, d - t};
}
cv::Rect expand(cv::Rect r, double margin, const cv::Size& size) {
    const int dx = static_cast<int>(std::nearbyint(r.width * margin));
    const int dy = static_cast<int>(std::nearbyint(r.height * margin));
    return cv::Rect(r.x-dx, r.y-dy, r.width+2*dx, r.height+2*dy) & cv::Rect(0,0,size.width,size.height);
}
Box read_box(const cv::FileNode& n) {
    if(!n.isSeq() || n.size() != 4) throw std::runtime_error("invalid ROI");
    Box b{}; int i = 0;
    for(const auto& v : n) {
        if(!v.isInt() && !v.isReal()) throw std::runtime_error("nonnumeric ROI coordinate");
        b[i++] = static_cast<double>(v);
    }
    for(double v : b) if(!std::isfinite(v) || v < 0 || v > 1) throw std::runtime_error("invalid ROI coordinate");
    if(b[0] >= b[2] || b[1] >= b[3]) throw std::runtime_error("empty ROI");
    return b;
}
double number(const cv::FileStorage& fs, const char* key, double lo, double hi) {
    const cv::FileNode n = fs[key];
    if(n.empty() || (!n.isInt() && !n.isReal())) throw std::runtime_error(std::string("missing or nonnumeric ") + key);
    const double v = static_cast<double>(n);
    if(!std::isfinite(v) || v < lo || v > hi) throw std::runtime_error(std::string("invalid ") + key);
    return v;
}
cv::Mat normalized(const cv::Mat& in) {
    cv::Mat out; cv::createCLAHE(2.0, cv::Size(4,4))->apply(in, out); return out;
}
cv::Mat bundled_template(const std::string& name) {
    for(const auto& asset : bundled_profile::kTemplates) {
        if(name == asset.name) {
            const cv::Mat bytes(1, static_cast<int>(asset.size), CV_8UC1,
                                const_cast<unsigned char*>(asset.data));
            return cv::imdecode(bytes, cv::IMREAD_GRAYSCALE);
        }
    }
    throw std::runtime_error("unknown bundled marker template: " + name);
}
}

const char* cabin_bundled_profile_id() { return bundled_profile::kId; }
const char* cabin_bundled_profile_sha256() { return bundled_profile::kSha256; }

struct CabinDetector::Impl {
    struct Marker { Box roi; cv::Mat image; };
    std::vector<Marker> markers;
    Box roi{};
    double aspect = 0, margin = .35, gray_weight = .7, high = .78, medium = .58,
           hidden = .42, blur = 15, dynamic = 35, over = .35, under = .35;
    int min_medium = 2;
};
CabinDetector::CabinDetector() : impl_(std::make_unique<Impl>()) {}
CabinDetector::~CabinDetector() = default;
bool CabinDetector::load_door_config(const std::string& path, std::string& error) {
    return load_door_source(path, false, error);
}
bool CabinDetector::load_bundled_door_config(std::string& error) {
    const auto& json = bundled_profile::kDoorJson;
    return load_door_source(std::string(reinterpret_cast<const char*>(json), sizeof(json)), true, error);
}
bool CabinDetector::load_door_source(const std::string& source, bool bundled, std::string& error) {
    door_calibrated_ = false;
    try {
        cv::FileStorage fs(source, cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON |
                                  (bundled ? cv::FileStorage::MEMORY : 0));
        if(!fs.isOpened()) throw std::runtime_error("door calibration unavailable");
        const double mean = number(fs,"max_mean_luma",0,40);
        const double p95 = number(fs,"max_p95_luma",0,50);
        const double dark = number(fs,"dark_pixel_threshold",0,60);
        const double fraction = number(fs,"min_dark_fraction",.95,1);
        const double clear_mean = number(fs,"clear_min_mean_luma",30,150);
        const double clear_p95 = number(fs,"clear_min_p95_luma",40,200);
        if(mean >= clear_mean || p95 >= clear_p95 || dark < p95)
            throw std::runtime_error("inconsistent door thresholds");
        door_max_mean_=mean; door_max_p95_=p95; door_dark_threshold_=dark;
        door_min_dark_fraction_=fraction; door_clear_mean_=clear_mean; door_clear_p95_=clear_p95;
        door_calibrated_ = true; error.clear(); return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
}
bool CabinDetector::load_plate_config(const std::string& path, std::string& error) {
    return load_plate_source(path, false, error);
}
bool CabinDetector::load_bundled_plate_config(std::string& error) {
    const auto& json = bundled_profile::kPlateJson;
    return load_plate_source(std::string(reinterpret_cast<const char*>(json), sizeof(json)), true, error);
}
bool CabinDetector::load_plate_source(const std::string& source, bool bundled, std::string& error) {
    impl_ = std::make_unique<Impl>();
    try {
        cv::FileStorage fs(source, cv::FileStorage::READ | cv::FileStorage::FORMAT_JSON |
                                  (bundled ? cv::FileStorage::MEMORY : 0));
        if(!fs.isOpened()) throw std::runtime_error("plate calibration unavailable");
        auto c = std::make_unique<Impl>();
        if(number(fs,"version",1,1) != 1) throw std::runtime_error("unsupported calibration version");
        c->roi = read_box(fs["search_roi"]);
        const auto size = fs["reference_size"];
        if(!size.isSeq() || size.size() != 2) throw std::runtime_error("reference_size required");
        for(const auto& value : size)
            if(!value.isInt() && !value.isReal()) throw std::runtime_error("nonnumeric reference size");
        const double w = static_cast<double>(size[0]), h = static_cast<double>(size[1]);
        if(!std::isfinite(w) || !std::isfinite(h) || w < 160 || h < 120 || w > 8192 || h > 8192 || w*h > kMaxPixels ||
           std::floor(w) != w || std::floor(h) != h || static_cast<int>(w)%2 || static_cast<int>(h)%2)
            throw std::runtime_error("invalid reference size");
        c->aspect = w / h;
        c->margin = number(fs,"search_margin",0,2);
        c->gray_weight = number(fs,"template_gray_weight",0,1);
        c->high = number(fs,"high_confidence_threshold",0,1);
        c->medium = number(fs,"medium_confidence_threshold",0,1);
        c->hidden = number(fs,"hidden_max_confidence",0,1);
        c->blur = number(fs,"min_blur_variance",1,100000);
        c->dynamic = number(fs,"min_dynamic_range",1,255);
        c->over = number(fs,"max_overexposed_fraction",0,.95);
        c->under = number(fs,"max_underexposed_fraction",0,.95);
        const double min_medium = number(fs,"min_medium_markers",1,32);
        if(std::floor(min_medium) != min_medium) throw std::runtime_error("min_medium_markers must be an integer");
        c->min_medium = static_cast<int>(min_medium);
        if(!(c->hidden < c->medium && c->medium <= c->high)) throw std::runtime_error("invalid confidence order");
        const auto list = fs["markers"];
        if(!list.isSeq() || list.empty() || list.size() > 32) throw std::runtime_error("markers required");
        if(c->min_medium > static_cast<int>(list.size())) throw std::runtime_error("min_medium_markers exceeds marker count");
        size_t template_pixels = 0;
        for(const auto& item : list) {
            Impl::Marker marker; marker.roi = read_box(item["roi"]);
            if(marker.roi[0] < c->roi[0] || marker.roi[1] < c->roi[1] ||
               marker.roi[2] > c->roi[2] || marker.roi[3] > c->roi[3])
                throw std::runtime_error("marker ROI outside search ROI");
            const cv::Rect expected = rect(marker.roi,static_cast<int>(w),static_cast<int>(h));
            if(expected.width < 12 || expected.height < 12) throw std::runtime_error("marker ROI too small");
            if(!item["template"].isString()) throw std::runtime_error("marker template must be a string");
            const std::string file = static_cast<std::string>(item["template"]);
            if(file.empty()) throw std::runtime_error("marker template required");
            const std::string image_path = bundled ? std::string("bundled:") + file :
                (std::filesystem::path(source).parent_path() / file).string();
            marker.image = bundled ? bundled_template(file) : cv::imread(image_path, cv::IMREAD_GRAYSCALE);
            if(marker.image.empty() || marker.image.cols < 12 || marker.image.rows < 12 ||
               marker.image.cols > w || marker.image.rows > h || marker.image.total() > kMaxPixels - template_pixels)
                throw std::runtime_error("invalid marker template: " + image_path);
            template_pixels += marker.image.total();
            cv::Scalar mean, dev; cv::meanStdDev(marker.image, mean, dev);
            if(dev[0] < 5) throw std::runtime_error("marker template lacks contrast");
            c->markers.push_back(std::move(marker));
        }
        impl_ = std::move(c); error.clear(); return true;
    } catch(const std::exception& e) { error = e.what(); return false; }
}

CabinDetection CabinDetector::door(const uint8_t* nv12, size_t bytes, uint32_t w,
                                  uint32_t h, uint8_t mask) const {
    if(!valid_nv12(nv12,bytes,w,h)) return {0,0,"invalid_nv12"};
    if(mask != 0x3f) return {0,0,"six_lamps_required"};
    if(!door_calibrated_) return {0,0,"door_calibration_unavailable"};
    try {
        cv::Mat y(static_cast<int>(h),static_cast<int>(w),CV_8UC1,const_cast<uint8_t*>(nv12));
        const double mean = cv::mean(y)[0], p95 = percentile(y,95);
        const double dark = static_cast<double>(cv::countNonZero(y <= door_dark_threshold_)) / y.total();
        // Includes limited-range NV12 black Y=16. Candidate thresholds from field clips.
        if(mean <= door_max_mean_ && p95 <= door_max_p95_ && dark >= door_min_dark_fraction_)
            return {1,mean,"full_black"};
        if(mean > door_clear_mean_ && p95 > door_clear_p95_ && cv::countNonZero(y >= 250) < y.total() * .35)
            return {2,mean,"scene_visible"};
        return {0,mean,"indeterminate_luminance"};
    } catch(const std::exception& e) { return {0,0,e.what()}; }
}

CabinDetection CabinDetector::plate(const uint8_t* nv12, size_t bytes, uint32_t w,
                                   uint32_t h, uint8_t mask) const {
    if(!valid_nv12(nv12,bytes,w,h)) return {0,0,"invalid_nv12"};
    if(mask != 0x3f) return {0,0,"six_lamps_required"};
    const auto& c = *impl_;
    if(c.markers.empty()) return {0,0,"plate_calibration_unavailable"};
    if(std::abs((static_cast<double>(w)/h) / c.aspect - 1) > .02) return {0,0,"aspect_changed"};
    try {
        cv::Mat raw(static_cast<int>(h*3/2),static_cast<int>(w),CV_8UC1,const_cast<uint8_t*>(nv12));
        cv::Mat bgr, gray; cv::cvtColor(raw,bgr,cv::COLOR_YUV2BGR_NV12);
        cv::cvtColor(bgr,gray,cv::COLOR_BGR2GRAY); // same luminance domain as PNG calibration
        const cv::Mat roi = gray(rect(c.roi,w,h));
        cv::Mat lap; cv::Laplacian(roi,lap,CV_64F);
        cv::Scalar mean,dev; cv::meanStdDev(lap,mean,dev);
        if(dev[0]*dev[0] < c.blur || percentile(roi,95)-percentile(roi,5) < c.dynamic ||
           cv::countNonZero(roi>=250) > roi.total()*c.over ||
           cv::countNonZero(roi<=5) > roi.total()*c.under) return {0,0,"scene_quality_failed"};
        double maximum = 0; int highs = 0, mediums = 0;
        for(const auto& marker : c.markers) {
            const cv::Rect expected = rect(marker.roi,w,h);
            if(expected.width < 12 || expected.height < 12) return {0,0,"marker_resolution_too_small"};
            const cv::Rect search_rect = expand(expected,c.margin,gray.size());
            const cv::Mat search = normalized(gray(search_rect));
            cv::Mat edges; cv::Canny(search,edges,50,150);
            double best = -1; cv::Rect best_rect = expected;
            for(double scale : {.90,.95,1.,1.05,1.10}) {
                const cv::Size size(std::max(12,static_cast<int>(std::nearbyint(expected.width*scale))),
                                    std::max(12,static_cast<int>(std::nearbyint(expected.height*scale))));
                if(size.width > search.cols || size.height > search.rows) continue;
                cv::Mat resized, norm, te, gm, em;
                cv::resize(marker.image,resized,size,0,0,cv::INTER_AREA);
                norm = normalized(resized); cv::Canny(norm,te,50,150);
                cv::matchTemplate(search,norm,gm,cv::TM_CCOEFF_NORMED);
                cv::matchTemplate(edges,te,em,cv::TM_CCOEFF_NORMED);
                cv::Mat combined = c.gray_weight*gm+(1-c.gray_weight)*em;
                cv::patchNaNs(combined,-1);
                double score; cv::Point point; cv::minMaxLoc(combined,nullptr,&score,nullptr,&point);
                if(std::isfinite(score) && score > best) {
                    best = score; best_rect = cv::Rect(search_rect.tl()+point,size);
                }
            }
            if(best < -0.999) return {0,0,"template_search_unavailable"};
            const cv::Mat patch = gray(best_rect);
            const cv::Rect surround_rect = expand(best_rect,.35,gray.size());
            const cv::Mat surround = gray(surround_rect);
            std::array<size_t,256> ring_hist{};
            size_t ring_size = 0;
            for(int y=0;y<surround.rows;++y) for(int x=0;x<surround.cols;++x)
                if(!best_rect.contains(cv::Point(x+surround_rect.x,y+surround_rect.y))) {
                    ++ring_hist[surround.at<uint8_t>(y,x)]; ++ring_size;
                }
            const double ring_median = ring_size == 0 ? percentile(surround,50) : percentile_hist(ring_hist,ring_size,50);
            cv::Mat pe; cv::Canny(normalized(patch),pe,50,150);
            const double density = static_cast<double>(cv::countNonZero(pe))/patch.total();
            double edge_component = clip((density-.02)/.08);
            if(density > .28) edge_component *= clip((.42-density)/.14);
            const double bright = static_cast<double>(cv::countNonZero(patch >= std::min(255.,ring_median+20)))/patch.total();
            const double structure = (clip((percentile(patch,50)-ring_median-8)/25) +
                clip((percentile(patch,90)-percentile(patch,10)-25)/45) +
                edge_component + clip((bright-.18)/.45))/4;
            const double confidence = clip(.72*clip((best-.25)/.65)+.28*structure);
            maximum = std::max(maximum,confidence);
            highs += confidence >= c.high; mediums += confidence >= c.medium;
        }
        if(highs || mediums >= c.min_medium) return {1,maximum,"calibrated_code_visible"};
        if(maximum <= c.hidden) return {2,maximum,"codes_absent_scene_ok"};
        return {0,maximum,"marker_hysteresis"};
    } catch(const std::exception& e) { return {0,0,e.what()}; }
}
} // namespace upc::visual
