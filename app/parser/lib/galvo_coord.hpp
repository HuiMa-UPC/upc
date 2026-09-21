#ifndef GALVO_COORD_HPP
#define GALVO_COORD_HPP

/**
 * @file galvo_coord.hpp
 * @brief 振镜坐标编码公共工具：mm 坐标 → 16-bit 振镜 DAC 偏移字节。
 *
 * 提取自 ovf_worker.cpp 匿名命名空间，供 OVF/G-code Worker 与 Glavo 手动控制
 * （McuLink::send_galvo_abs_pos）共用，保证切片下发与手动移动编码口径一致。
 *
 * 编码口径（与 coordinate_to_offset_bytes 历史实现完全一致）：
 *   1. 限幅到 [-image_rang_gain, +image_rang_gain]（默认 ±100mm）
 *   2. 归一化 normalized = coord / image_rang_gain ∈ [-1, +1]
 *   3. 映射到 16-bit：offset = normalized × 32767 + 32768 ∈ [1, 65535]
 *      中心 0 → 32768，负向 → 1，正向 → 65535
 *   4. 拆成 LE 两字节输出
 */

#include <cstdint>

namespace upc::parser {

/**
 * @brief 将整型值钳位到 [0, 65535] 后拆成 LE 两字节。
 * @param value      输入（越界自动钳位）
 * @param low_byte   输出低字节
 * @param high_byte  输出高字节
 */
void int_to_u16_bytes(int value, uint8_t* low_byte, uint8_t* high_byte);

/**
 * @brief mm 坐标 → 16-bit 振镜 DAC 偏移（LE 两字节）。
 *
 * @param coordinate_value  目标坐标(mm，振镜坐标系，0 为中心)
 * @param image_rang_gain    归一化基准范围(mm)，<=0 时用默认 100mm；
 *                          坐标越界自动截断到 [-range, +range]
 * @param low_byte          输出 X/Y 低字节
 * @param high_byte         输出 X/Y 高字节
 */
void coordinate_to_offset_bytes(float coordinate_value, float image_rang_gain,
                                uint8_t* low_byte, uint8_t* high_byte);

} // namespace upc::parser

#endif // GALVO_COORD_HPP
