/**
 * @file height_lut_2d.cpp
 * @brief 多点振镜校准的二维高度查表实现。
 */

#include "height_lut_2d.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

namespace upc::glavo {

namespace {

std::string trim(const std::string& s) {
    const size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

/** 用 (galvo_x, galvo_y) 作为分组键（浮点近似相等）。 */
struct GalvoKey {
    float x, y;
    bool operator<(const GalvoKey& o) const {
        if (x != o.x) return x < o.x;
        return y < o.y;
    }
};

/** 对一组 (h, img_x, img_y) 样本做最小二乘线性回归，求 (a0,a1,b0,b1)。 */
bool regress(const std::vector<std::array<float, 3>>& pts,  // [h, img_x, img_y]
             GalvoLutEntry& out)
{
    const int n = static_cast<int>(pts.size());
    if (n < 2) {
        return false;
    }
    double sh = 0, sx = 0, sy = 0;
    for (const auto& p : pts) {
        sh += p[0]; sx += p[1]; sy += p[2];
    }
    const double mh = sh / n, mx = sx / n, my = sy / n;
    double sxx = 0, syy = 0, shh = 0;
    for (const auto& p : pts) {
        const double dh = p[0] - mh;
        sxx += dh * (p[1] - mx);
        syy += dh * (p[2] - my);
        shh += dh * dh;
    }
    if (std::fabs(shh) < 1e-12) {
        return false;
    }
    out.a1 = static_cast<float>(sxx / shh);
    out.a0 = static_cast<float>(mx - out.a1 * mh);
    out.b1 = static_cast<float>(syy / shh);
    out.b0 = static_cast<float>(my - out.b1 * mh);
    out.count = n;
    return true;
}

} // namespace

bool HeightLut2d::load(const std::string& path) {
    return load(path, -1.0e9f, 1.0e9f);
}

bool HeightLut2d::load(const std::string& path, float h_min, float h_max) {
    entries_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        UPC_LOG_GLAVO_SERVICE(Error, "HeightLut2d: open failed: %s", path.c_str());
        return false;
    }

    std::map<GalvoKey, std::vector<std::array<float, 3>>> groups;
    std::string line;
    int total = 0;
    while (std::getline(file, line)) {
        const std::string str = trim(line);
        if (str.empty() || str.front() == '#') {
            continue;
        }
        std::istringstream iss(str);
        float h, gx, gy, ix, iy;
        if (!(iss >> h >> gx >> gy >> ix >> iy)) {
            continue;
        }
        if (h < h_min - 1e-4f || h > h_max + 1e-4f) {
            continue;
        }
        groups[{gx, gy}].push_back({h, ix, iy});
        ++total;
    }

    if (groups.empty()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "HeightLut2d: no valid rows in %s", path.c_str());
        return false;
    }

    for (const auto& [key, pts] : groups) {
        GalvoLutEntry e{};
        e.galvo_x = key.x;
        e.galvo_y = key.y;
        if (!regress(pts, e)) {
            UPC_LOG_GLAVO_SERVICE(Warn,
                "HeightLut2d: regress failed at (%.1f,%.1f) n=%zu, skip",
                static_cast<double>(key.x), static_cast<double>(key.y), pts.size());
            continue;
        }
        entries_.push_back(e);
    }

    if (entries_.empty()) {
        UPC_LOG_GLAVO_SERVICE(Warn, "HeightLut2d: no valid groups after regress");
        return false;
    }

    UPC_LOG_GLAVO_SERVICE(Info,
        "HeightLut2d loaded: %s rows=%d groups=%zu",
        path.c_str(), total, entries_.size());
    for (const auto& e : entries_) {
        UPC_LOG_GLAVO_SERVICE(Debug,
            "  lut (%.1f,%.1f) n=%d a0=%.3f a1=%.4f b0=%.3f b1=%.4f",
            static_cast<double>(e.galvo_x), static_cast<double>(e.galvo_y),
            e.count, static_cast<double>(e.a0), static_cast<double>(e.a1),
            static_cast<double>(e.b0), static_cast<double>(e.b1));
    }
    return true;
}

float HeightLut2d::lookup(float galvo_x, float galvo_y,
                          float img_x, float img_y) const {
    if (entries_.empty()) {
        return std::nanf("");
    }

    const GalvoLutEntry* best = nullptr;
    float best_d2 = std::numeric_limits<float>::max();
    for (const auto& e : entries_) {
        const float dx = e.galvo_x - galvo_x;
        const float dy = e.galvo_y - galvo_y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = &e;
        }
    }
    if (!best) {
        return std::nanf("");
    }

    const float denom = best->a1 * best->a1 + best->b1 * best->b1;
    if (denom < 1e-12f) {
        return std::nanf("");
    }
    const float h = (best->a1 * (img_x - best->a0) +
                     best->b1 * (img_y - best->b0)) / denom;
    return h;
}

} // namespace upc::glavo
