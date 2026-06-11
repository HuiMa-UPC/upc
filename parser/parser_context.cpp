#include "parser_context.hpp"
#include "parser_config.hpp"
#include "rs_log.hpp"

#include <cstdio>

namespace upc::parser {

ParserContext& ParserContext::instance()
{
    static ParserContext ctx;
    return ctx;
}

void ParserContext::load_calibration(const char* path)
{
    cal_data_clear(&cal_data_);
    cal_valid_ = false;

    if(path && cal_parse_csv(path, &cal_data_) == 0 && cal_data_.valid) {
        cal_valid_ = true;
        if(cal_data_.has_gains) {
            scale_gain_ = cal_data_.scale_gain;
            range_gain_  = cal_data_.range_gain;
            speed_gain_  = cal_data_.speed_gain;
        } else {
            scale_gain_ = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
            range_gain_  = GCODE_DEFAULT_IMAGE_RANG_GAIN;
            speed_gain_  = GCODE_DEFAULT_SPEED_GAIN;
        }
        UPC_LOG_PARSER_CONTEXT(Info,
            "CAL loaded: scale=%.3f range=%.3f speed=%.3f grid=%dx%d",
            scale_gain_, range_gain_, speed_gain_, cal_data_.nx, cal_data_.ny);
    } else {
        scale_gain_ = GCODE_DEFAULT_IMAGE_SCALE_GAIN;
        range_gain_  = GCODE_DEFAULT_IMAGE_RANG_GAIN;
        speed_gain_  = GCODE_DEFAULT_SPEED_GAIN;
        UPC_LOG_PARSER_CONTEXT(Warn,
            "CAL load failed, using defaults: scale=%.3f range=%.3f speed=%.3f",
            scale_gain_, range_gain_, speed_gain_);
    }
}

} // namespace upc::parser
