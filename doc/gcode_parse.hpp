#ifndef GCODE_PARSE_H
#define GCODE_PARSE_H

#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/** 默认与 docs/Qt、下位机 spead_gain 一致 */
#define GCODE_DEFAULT_IMAGE_SCALE_GAIN  1.0f
#define GCODE_DEFAULT_IMAGE_RANG_GAIN   100.0f
#define GCODE_DEFAULT_SPEED_GAIN        167.12f

/**
 * 运动点坐标变换调试：1=开启，0=关闭（也可在编译选项 -DGCODE_MOTION_COORD_DEBUG=1）
 * 每个 G0/G1 运动包打印：解析 XY、校准后 XY、中心偏移后 XY、乘 scale 后 XY、Range 映射 u16。
 */
#ifndef GCODE_MOTION_COORD_DEBUG
#define GCODE_MOTION_COORD_DEBUG 0
#endif

#if GCODE_MOTION_COORD_DEBUG
#include <stdio.h>
#define GCODE_MOTION_COORD_DBG(fmt, ...)                                                      \
    do {                                                                                      \
        fprintf(stderr, "[gcode_motion] " fmt "\n", ##__VA_ARGS__);                           \
        fflush(stderr);                                                                       \
    } while(0)
#else
#define GCODE_MOTION_COORD_DBG(fmt, ...) ((void)0)
#endif

/** 将 Control 页 Parse 增益恢复为默认值（写入 g_job_status） */
void gcode_parse_gains_set_defaults(void);

/** 当前生效的 image scale/range（CAL 使能时用 CAL 增益，否则用 Parse 增益） */
void gcode_get_active_image_gains(float *scale_gain, float *rang_gain);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <vector>

/**
 * 振镜平面校准数据（对齐 docs/Qt/galvocalibration.h）
 * 标定域以网格几何中心为 G 代码原点 (0,0)
 */
struct galvo_cal_data_t {
    bool valid = false;
    int nx = 0;
    int ny = 0;
    double grid_x = 0.0;
    double grid_y = 0.0;
    double x_min = 0.0;
    double x_max = 0.0;
    double y_min = 0.0;
    double y_max = 0.0;
    std::vector<double> z_dx; /**< row-major: z_dx[i + j*nx] */
    std::vector<double> z_dy;

    bool has_cal_gains = false;
    float cal_image_scale_gain = 0.f;
    float cal_image_rang_gain = 0.f;
    float cal_speed_gain = 0.f;
};

void galvo_cal_clear(galvo_cal_data_t *cal);

/** 解析 CSV；失败时可选写入 err（UTF-8 英文短消息） */
int galvo_cal_parse_csv(const char *path, galvo_cal_data_t *out,
                        char *err, size_t err_size);

/** 在 (x,y) 双线性插值 ΔX/ΔY；成功返回 1，失败 0 */
int galvo_cal_interpolate_delta(const galvo_cal_data_t *cal,
                                double x, double y,
                                double *out_dx, double *out_dy);

/** 线程安全：更新全局已加载校准 */
void galvo_cal_store(const galvo_cal_data_t *cal);

/** 线程安全：拷贝全局校准到 out */
void galvo_cal_copy(galvo_cal_data_t *out);

/** 线程安全：全局校准是否 valid */
int galvo_cal_is_valid(void);

#endif /* __cplusplus */

#endif
