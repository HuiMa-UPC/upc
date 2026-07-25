#include "config/cal_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace upc::config {

namespace {

void trim_inplace(std::string& s)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
}

std::string to_upper_copy(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

bool parse_bool_like(const std::string& raw, bool* out)
{
    if(!out) {
        return false;
    }
    std::string v = to_upper_copy(raw);
    trim_inplace(v);
    if(v.empty()) {
        return false;
    }
    if(v == "1" || v == "TRUE" || v == "YES" || v == "Y" || v == "ON") {
        *out = true;
        return true;
    }
    if(v == "0" || v == "FALSE" || v == "NO" || v == "N" || v == "OFF") {
        *out = false;
        return true;
    }
    return false;
}

bool parse_float_like(const std::string& raw, float* out)
{
    if(!out) {
        return false;
    }
    std::string v = raw;
    trim_inplace(v);
    if(v.empty()) {
        return false;
    }
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    if(end == v.c_str() || (end && *end != '\0')) {
        return false;
    }
    *out = f;
    return true;
}

bool split_key_value(const char* line, std::string& key, std::string& value)
{
    if(!line) {
        return false;
    }
    const char* comma1 = std::strchr(line, ',');
    if(!comma1) {
        key.assign(line);
        value.clear();
    } else {
        key.assign(line, static_cast<size_t>(comma1 - line));
        const char* value_begin = comma1 + 1;
        const char* comma2 = std::strchr(value_begin, ',');
        if(comma2) {
            value.assign(value_begin, static_cast<size_t>(comma2 - value_begin));
        } else {
            value.assign(value_begin);
        }
    }
    trim_inplace(key);
    trim_inplace(value);
    return !key.empty();
}

} // namespace

bool load_cal_config_options(const char* path, CalConfigOptions* out)
{
    if(!path || !out) {
        return false;
    }
    *out = CalConfigOptions{};

    FILE* fp = std::fopen(path, "r");
    if(!fp) {
        return false;
    }

    char line_buf[4096];
    bool first_line = true;
    bool in_config = false;
    while(std::fgets(line_buf, sizeof(line_buf), fp)) {
        if(first_line) {
            first_line = false;
            if(static_cast<unsigned char>(line_buf[0]) == 0xEF &&
               static_cast<unsigned char>(line_buf[1]) == 0xBB &&
               static_cast<unsigned char>(line_buf[2]) == 0xBF) {
                std::memmove(line_buf, line_buf + 3, std::strlen(line_buf + 3) + 1);
            }
        }

        std::string key;
        std::string value;
        if(!split_key_value(line_buf, key, value)) {
            continue;
        }

        const std::string key_upper = to_upper_copy(key);
        if(!in_config) {
            if(key_upper == "CONFIG") {
                in_config = true;
                out->has_config_section = true;
            }
            continue;
        }

        if(key_upper == "GLAVO_DOOR_REAL_OPEN_ENABLE") {
            bool v = false;
            if(parse_bool_like(value, &v)) {
                out->has_glavo_door_real_open_enable = true;
                out->glavo_door_real_open_enable = v;
            }
            continue;
        }
        if(key_upper == "GLAVO_DOOR_REAL_CLOSE_CHECK_ENABLE") {
            bool v = false;
            if(parse_bool_like(value, &v)) {
                out->has_glavo_door_real_close_check_enable = true;
                out->glavo_door_real_close_check_enable = v;
            }
            continue;
        }
        if(key_upper == "GLAVO_SERIAL_ACK_ENABLE") {
            bool v = false;
            if(parse_bool_like(value, &v)) {
                out->has_glavo_serial_ack_enable = true;
                out->glavo_serial_ack_enable = v;
            }
            continue;
        }
        if(key_upper == "PM2.5CLEAN") {
            float v = 0.0f;
            if(parse_float_like(value, &v)) {
                out->has_pm25_clean_threshold = true;
                out->pm25_clean_threshold = v;
            }
            continue;
        }
        if(key_upper == "DEVICE_CN") {
            out->has_device_cn = true;
            out->device_cn = value;
            continue;
        }
        if(key_upper == "DEVICE MODEL") {
            out->has_device_model = true;
            out->device_model = value;
            continue;
        }
    }

    std::fclose(fp);
    return true;
}

} // namespace upc::config
