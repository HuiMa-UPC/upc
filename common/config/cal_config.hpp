#ifndef UPC_CAL_CONFIG_HPP
#define UPC_CAL_CONFIG_HPP

#include <cstdint>
#include <string>

namespace upc::config {

struct CalConfigOptions {
    bool has_config_section = false;

    bool has_glavo_serial_ack_enable = false;
    bool glavo_serial_ack_enable = false;

    bool has_pm25_clean_threshold = false;
    float pm25_clean_threshold = 0.0f;

    bool has_device_cn = false;
    std::string device_cn;

    bool has_device_model = false;
    std::string device_model;

    /* TIMELAPSE_VIDEO_FPS / PRINT_H264_BITRATE 不再解析。
     * 延时帧率固定 CAMERA_PRINT_VIDEO_LAYER_FPS（5），
     * 打印录像码率固定 CAMERA_PRINT_H264_BITRATE（2048000）。 */
};

/** 默认校准目录 / 文件（部署路径；parser 独立构建见 parser_config.hpp 同名宏） */
#ifndef UPC_CAL_DIR
#define UPC_CAL_DIR  "/userdata/CAL"
#endif

#ifndef UPC_CAL_PATH
#define UPC_CAL_PATH  UPC_CAL_DIR "/cal.csv"
#endif

/** 默认校准文件路径（部署路径） */
constexpr const char* kCalDefaultPath = UPC_CAL_PATH;

/** UI 顶栏等场景使用的设备显示名。 */
inline constexpr const char* kDeviceDisplayName = "SNOWPOD A1";

/** 读取 cal.csv 中 Config 段可选项。无 Config 段时返回 true 且 has_config_section=false。 */
bool load_cal_config_options(const char* path, CalConfigOptions* out);

} // namespace upc::config

#endif // UPC_CAL_CONFIG_HPP
