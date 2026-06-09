#ifndef OVF_CONFIG_HPP
#define OVF_CONFIG_HPP

#include "device_config.hpp"

/** OVF 默认层厚（μm），WorkPlane 无 z_pos_in_mm 时使用 */
#ifndef OVF_DEFAULT_LAYER_UM
#define OVF_DEFAULT_LAYER_UM  GCODE_DEFAULT_LAYER_UM
#endif

#ifndef OVF_PARSE_CHUNK_SEGMENTS
#define OVF_PARSE_CHUNK_SEGMENTS        100
#endif

#ifndef OVF_PARSE_SEND_SYNC_SEGMENTS
#define OVF_PARSE_SEND_SYNC_SEGMENTS    50
#endif

#ifndef OVF_PARSE_CHUNK_INTERVAL_MS
#define OVF_PARSE_CHUNK_INTERVAL_MS     10
#endif

/** 默认进给（mm/min），MarkingParams 未解析时使用 */
#ifndef OVF_DEFAULT_FEED_MM_MIN
#define OVF_DEFAULT_FEED_MM_MIN         1200.0f
#endif

#endif // OVF_CONFIG_HPP
