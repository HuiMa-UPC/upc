#include "cal_parser.hpp"
#include "parser_config.hpp"
#include "rs_log.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <vector>
#include <map>

namespace upc::parser {

/* ---------- 内部辅助 ---------- */

static void trim_inplace(char *s)
{
    if (!s) return;
    char *start = s;
    while (*start && std::isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t len = strlen(s);
    while (len > 0 && std::isspace(static_cast<unsigned char>(s[len - 1]))) {
        s[--len] = '\0';
    }
}

static int ci_starts_with(const char *line, const char *prefix)
{
    if (!line || !prefix) return 0;
    while (*prefix) {
        if (std::tolower(static_cast<unsigned char>(*line)) !=
            std::tolower(static_cast<unsigned char>(*prefix))) {
            return 0;
        }
        ++line;
        ++prefix;
    }
    return 1;
}

static double bilinear_interp(const std::vector<double> &z, int nx, int ny,
                              double x_min, double x_max,
                              double y_min, double y_max,
                              double x, double y)
{
    if (nx < 2 || ny < 2 || (int)z.size() != nx * ny) {
        return 0.0;
    }

    if (x < x_min) x = x_min;
    if (x > x_max) x = x_max;
    if (y < y_min) y = y_min;
    if (y > y_max) y = y_max;

    const double dx = (x_max - x_min) / double(nx - 1);
    const double dy = (y_max - y_min) / double(ny - 1);
    const double idx = 1.0 / dx;
    const double idy = 1.0 / dy;

    double fx = (x - x_min) * idx;
    double fy = (y - y_min) * idy;

    int i = static_cast<int>(fx);
    int j = static_cast<int>(fy);
    if (i >= nx - 1) i = nx - 2;
    if (j >= ny - 1) j = ny - 2;

    const double tx = fx - double(i);
    const double ty = fy - double(j);

    const int i00 = i + j * nx;
    const double v00 = z[(size_t)i00];
    const double v10 = z[(size_t)(i00 + 1)];
    const double v01 = z[(size_t)(i00 + nx)];
    const double v11 = z[(size_t)(i00 + nx + 1)];

    const double c0 = v00 + tx * (v10 - v00);
    const double c1 = v01 + tx * (v11 - v01);
    return c0 + ty * (c1 - c0);
}

/* ---------- 公开接口 ---------- */

void cal_data_clear(CalData *cal)
{
    if (!cal) return;
    *cal = CalData{};
}

int cal_parse_csv(const char *path, CalData *out)
{
    if (!path || !out) {
        return -1;
    }
    cal_data_clear(out);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        UPC_LOG_CAL_PARSER(Error, "fopen failed: %s", path);
        return -1;
    }

    int nx = 0;
    int ny = 0;
    double grid_x = 0.0;
    double grid_y = 0.0;
    bool has_cal_gains = false;
    float cal_scale = 0.f;
    float cal_range = 0.f;
    float cal_speed = 0.f;

    std::map<int, std::vector<double>> x_rows;
    std::map<int, std::vector<double>> y_rows;

    char line_buf[4096];
    bool first_line = true;
    while (fgets(line_buf, sizeof(line_buf), fp)) {
        if (first_line) {
            first_line = false;
            if ((unsigned char)line_buf[0] == 0xEF &&
                (unsigned char)line_buf[1] == 0xBB &&
                (unsigned char)line_buf[2] == 0xBF) {
                memmove(line_buf, line_buf + 3, strlen(line_buf + 3) + 1);
            }
        }
        trim_inplace(line_buf);
        if (line_buf[0] == '\0') {
            continue;
        }

        if (ci_starts_with(line_buf, "CAL")) {
            double s = 0.0;
            double r = 0.0;
            double sp = 0.0;
            if (sscanf(line_buf, "CAL,%lf,%lf,%lf", &s, &r, &sp) == 3 ||
                sscanf(line_buf, "cal,%lf,%lf,%lf", &s, &r, &sp) == 3) {
                has_cal_gains = true;
                cal_scale = static_cast<float>(s);
                cal_range = static_cast<float>(r);
                cal_speed = static_cast<float>(sp);
            }
            continue;
        }

        if (ci_starts_with(line_buf, "Table")) {
            double gx = 0.0;
            double gy = 0.0;
            if (sscanf(line_buf, "Table,%d,%d,%lf,%lf", &nx, &ny, &gx, &gy) == 4 ||
                sscanf(line_buf, "table,%d,%d,%lf,%lf", &nx, &ny, &gx, &gy) == 4) {
                grid_x = gx;
                grid_y = gy;
            }
            continue;
        }

        char label[32];
        label[0] = '\0';
        if (sscanf(line_buf, "%31[^,]", label) != 1) {
            continue;
        }
        trim_inplace(label);
        if (label[0] == '\0') {
            continue;
        }

        char axis = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        if ((axis != 'X' && axis != 'Y') || !std::isdigit(static_cast<unsigned char>(label[1]))) {
            continue;
        }
        const int row_idx = std::atoi(label + 1);
        if (row_idx <= 0) {
            continue;
        }

        std::vector<double> vals;
        const char *p = strchr(line_buf, ',');
        while (p && *p) {
            ++p;
            while (*p && std::isspace(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (*p == '\0') {
                break;
            }
            char *end = nullptr;
            const double v = std::strtod(p, &end);
            if (end == p) {
                break;
            }
            vals.push_back(v);
            p = end;
        }

        if (axis == 'X') {
            x_rows[row_idx] = std::move(vals);
        } else {
            y_rows[row_idx] = std::move(vals);
        }
    }
    fclose(fp);

    if (nx < 2 || ny < 2 || grid_x <= 0.0 || grid_y <= 0.0) {
        UPC_LOG_CAL_PARSER(Error,
            "parse failed: missing valid Table row (nx=%d ny=%d gx=%.3f gy=%.3f)",
            nx, ny, grid_x, grid_y);
        return -1;
    }
    if (nx > 64 || ny > 64) {
        UPC_LOG_CAL_PARSER(Error, "parse failed: grid too large (%dx%d)", nx, ny);
        return -1;
    }

    std::vector<double> z_dx;
    std::vector<double> z_dy;
    z_dx.resize((size_t)nx * (size_t)ny);
    z_dy.resize((size_t)nx * (size_t)ny);

    for (int j = 0; j < ny; ++j) {
        const int label = j + 1;
        auto itx = x_rows.find(label);
        auto ity = y_rows.find(label);
        if (itx == x_rows.end() || ity == y_rows.end()) {
            UPC_LOG_CAL_PARSER(Error, "parse failed: missing X%d or Y%d row", label, label);
            return -1;
        }
        if ((int)itx->second.size() != nx || (int)ity->second.size() != nx) {
            UPC_LOG_CAL_PARSER(Error,
                "parse failed: row %d column count mismatch (got %zu/%zu, expect %d)",
                label, itx->second.size(), ity->second.size(), nx);
            return -1;
        }
        for (int i = 0; i < nx; ++i) {
            const size_t idx = (size_t)i + (size_t)j * (size_t)nx;
            z_dx[idx] = itx->second[(size_t)i];
            z_dy[idx] = ity->second[(size_t)i];
        }
    }

    const double half_span_x = 0.5 * double(nx - 1) * grid_x;
    const double half_span_y = 0.5 * double(ny - 1) * grid_y;

    out->valid = true;
    out->nx = nx;
    out->ny = ny;
    out->grid_x = grid_x;
    out->grid_y = grid_y;
    out->x_min = -half_span_x;
    out->x_max = +half_span_x;
    out->y_min = -half_span_y;
    out->y_max = +half_span_y;
    out->z_dx = std::move(z_dx);
    out->z_dy = std::move(z_dy);
    out->has_gains = has_cal_gains;
    out->scale_gain = cal_scale;
    out->range_gain = cal_range;
    out->speed_gain = cal_speed;

    UPC_LOG_CAL_PARSER(Info,
        "parsed OK: nx=%d ny=%d grid=(%.3f,%.3f) has_gains=%d",
        out->nx, out->ny, out->grid_x, out->grid_y, out->has_gains);
    return 0;
}

int cal_interpolate_delta(const CalData *cal, double x, double y,
                          double *out_dx, double *out_dy)
{
    if (!cal || !cal->valid || !out_dx || !out_dy) {
        return 0;
    }
    if ((int)cal->z_dx.size() != cal->nx * cal->ny ||
        (int)cal->z_dy.size() != cal->nx * cal->ny) {
        return 0;
    }
    *out_dx = bilinear_interp(cal->z_dx, cal->nx, cal->ny,
                              cal->x_min, cal->x_max, cal->y_min, cal->y_max, x, y);
    *out_dy = bilinear_interp(cal->z_dy, cal->nx, cal->ny,
                              cal->x_min, cal->x_max, cal->y_min, cal->y_max, x, y);
    return 1;
}

} // namespace upc::parser
