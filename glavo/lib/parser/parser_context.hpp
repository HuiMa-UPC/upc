#ifndef PARSER_CONTEXT_HPP
#define PARSER_CONTEXT_HPP

#include "cal_parser.hpp"

namespace services {

/** 启动时加载一次的共享校准/增益数据，各协议解析器按需读取 */
class ParserContext {
public:
    static ParserContext& instance();

    void load_calibration(const char* path);

    const CalData& cal_data() const { return cal_data_; }
    float scale_gain() const { return scale_gain_; }
    float range_gain() const { return range_gain_; }
    float speed_gain() const { return speed_gain_; }
    bool  cal_valid() const { return cal_valid_; }

private:
    ParserContext() = default;

    CalData cal_data_;
    float   scale_gain_ = 0.f;
    float   range_gain_  = 0.f;
    float   speed_gain_  = 0.f;
    bool    cal_valid_   = false;
};

} // namespace services

#endif // PARSER_CONTEXT_HPP
