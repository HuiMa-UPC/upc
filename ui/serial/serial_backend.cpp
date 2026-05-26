#include "serial_backend.hpp"
#include "mcu_protocol.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/select.h>

/* ============================================================================
 *  内部状态
 * ============================================================================ */
static int              s_fd        = -1;
static char             s_device[64];
static int              s_baud      = SERIAL_DEFAULT_BAUD;
static pthread_t        s_rx_thread = 0;
static volatile bool    s_rx_running = false;

static pthread_mutex_t  s_state_mtx = PTHREAD_MUTEX_INITIALIZER;
static serial_status_t  s_status;

static pthread_mutex_t  s_tx_mtx    = PTHREAD_MUTEX_INITIALIZER;

#define STATE_LOCK()    pthread_mutex_lock(&s_state_mtx)
#define STATE_UNLOCK()  pthread_mutex_unlock(&s_state_mtx)
#define TX_LOCK()       pthread_mutex_lock(&s_tx_mtx)
#define TX_UNLOCK()     pthread_mutex_unlock(&s_tx_mtx)

/* ============================================================================
 *  低层串口辅助
 * ============================================================================ */
static speed_t baud_to_speed(int baud)
{
    switch(baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B115200;
    }
}

static int configure_port(int fd, int baud)
{
    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if(tcgetattr(fd, &tty) != 0) {
        return -1;
    }

    speed_t spd = baud_to_speed(baud);
    cfsetispeed(&tty, spd);
    cfsetospeed(&tty, spd);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | INLCR | IGNCR | ICRNL);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if(tcsetattr(fd, TCSANOW, &tty) != 0) {
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return 0;
}

/* ============================================================================
 *  RX 帧分发
 *  ----------------------------------------------------------------------------
 *  mcu_proto_feed_byte() 解析出完整帧后回调 on_mcu_frame()，按 (FrameType,
 *  Port, Cmd, TLV-Type) 分发到状态字段。
 *
 *  当前实现的上报：
 *    [0xBB] PORT_MOTION  / CMD_MOTION_STATUS  / TLV_MOTION_ACTUAL_POS (16B)
 *           → z_pos / s_pos / d_pos / z_work_pos  (保留 int32 原始单位)
 *               z/z_work: 0.1um   s: 0.1mm   d: 0.1°
 *    [0xBB] PORT_SENSORS / CMD_SENSOR_VALUE_REPORT / TLV_SENSOR_REPORT_1ST (13B)
 *           → o2_pct / chamber_kpa / temp_c / humidity_pct / pm25_ugm3
 *               单位换算后存 float（mbar → kPa, 0.1℃ → ℃, 0.1% → %）
 *    [0xBB] PORT_GENERAL / CMD_MOTOR_ENABLE / TLV_MOTOR_ENABLED/DISENABLED/FAILED
 *           → enabled
 *
 *  其余帧（ACK 0xBA、报警快照、POST_RESULT 等）当前只递增 rx_lines，未来按需扩展。
 * ============================================================================ */
static void on_mcu_frame(const mcu_frame_t *frm, void *user)
{
    (void)user;
    STATE_LOCK();
    s_status.rx_lines++;
    STATE_UNLOCK();

    if(!frm) return;
    /* 当前只处理 MCU 主动上报；ACK 暂忽略（Seq 关联未启用） */
    if(frm->frame_type != MCU_FT_REPORT) {
        return;
    }

    const uint8_t  cmd     = frm->cmd;
    const uint8_t  port    = frm->port;
    const uint8_t *pl      = frm->payload;
    const uint8_t  pl_len  = frm->payload_len;

    /* === Motion 实际位置周期上报（16B Value） === */
    if(port == MCU_PORT_MOTION && cmd == MCU_CMD_MOTION_STATUS
       && pl_len >= 2 + 16
       && pl[0] == MCU_TLV_MOTION_ACTUAL_POS
       && pl[1] >= 16) {
        const uint8_t *v = pl + 2;
        STATE_LOCK();
        s_status.z_pos       = mcu_rd_i32_le(v + 0);    /* 0.1um  */
        s_status.s_pos       = mcu_rd_i32_le(v + 4);    /* 0.1mm  */
        s_status.d_pos       = mcu_rd_i32_le(v + 8);    /* 0.1°   */
        s_status.z_work_pos  = mcu_rd_i32_le(v + 12);   /* 0.1um  */
        s_status.z_pos_valid = true;
        s_status.s_pos_valid = true;
        s_status.d_pos_valid = true;
        s_status.z_work_valid = true;
        STATE_UNLOCK();
        return;
    }

    /* === Sensor 周期快照（13B Value） === */
    if(port == MCU_PORT_SENSORS && cmd == MCU_CMD_SENSOR_VALUE_REPORT
       && pl_len >= 2 + 13
       && pl[0] == MCU_TLV_SENSOR_REPORT_1ST
       && pl[1] >= 13) {
        const uint8_t *v = pl + 2;
        const int16_t o2_x10 = mcu_rd_i16_le(v + 0);   /* 0.1 %    */
        const int16_t pmbar  = mcu_rd_i16_le(v + 2);   /* mbar     */
        const int16_t t_x10  = mcu_rd_i16_le(v + 4);   /* 0.1 ℃   */
        const int16_t h_x10  = mcu_rd_i16_le(v + 6);   /* 0.1 %    */
        const int16_t pm     = mcu_rd_i16_le(v + 8);   /* ug/m³    */
        /* v[10]   接近开关粉料状态 (0/1)   —— 暂无 UI 字段，先忽略
         * v[11..12] 基板温度 0.1℃          —— 暂无 UI 字段，先忽略
         */
        STATE_LOCK();
        s_status.o2_pct        = (float)o2_x10 / 10.0f;
        s_status.chamber_kpa   = (float)pmbar * 0.1f;   /* mbar → kPa */
        s_status.temp_c        = (float)t_x10  / 10.0f;
        s_status.humidity_pct  = (float)h_x10  / 10.0f;
        s_status.pm25_ugm3     = (float)pm;
        s_status.o2_valid       = true;
        s_status.chamber_valid  = true;
        s_status.temp_valid     = true;
        s_status.humidity_valid = true;
        s_status.pm25_valid     = true;
        STATE_UNLOCK();
        return;
    }

    /* === 电机使能/失能结果反馈 === */
    if(port == MCU_PORT_GENERAL && cmd == MCU_CMD_MOTOR_ENABLE
       && pl_len >= 2) {
        const uint8_t type = pl[0];
        STATE_LOCK();
        if(type == MCU_TLV_MOTOR_ENABLED) {
            s_status.enabled = true;
        } else if(type == MCU_TLV_MOTOR_DISENABLED
               || type == MCU_TLV_MOTOR_ENABLE_FAILED) {
            s_status.enabled = false;
        }
        STATE_UNLOCK();
        return;
    }

    /* 其余帧（POST_RESULT / WAKE / ALARM 等）暂不处理 */
}

/* ============================================================================
 *  RX 线程：阻塞 select + 字节流喂入协议状态机
 * ============================================================================ */
static void *rx_thread_fn(void *)
{
    while(s_rx_running) {
        int fd = s_fd;
        if(fd < 0) {
            usleep(10 * 1000);
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
        int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
        if(rv <= 0) {
            continue;
        }

        uint8_t buf[256];
        ssize_t n = read(fd, buf, sizeof(buf));
        if(n <= 0) {
            continue;
        }

        STATE_LOCK();
        s_status.rx_bytes += (uint32_t)n;
        STATE_UNLOCK();

        for(ssize_t i = 0; i < n; i++) {
            mcu_proto_feed_byte(buf[i], on_mcu_frame, NULL);
        }
    }
    return NULL;
}

/* ============================================================================
 *  TX 通用工具
 *  ----------------------------------------------------------------------------
 *  组帧逻辑全部移到 mcu_protocol.cpp。本文件只保留：
 *    - 类型边界转换（float → int16）
 *    - 串口实际写入（带锁 + TX 字节统计）
 * ============================================================================ */

/** 把 float 安全转成 int16，超界截断 */
static int16_t to_int16_clamp(float f)
{
    if(f >  32767.0f) return  32767;
    if(f < -32768.0f) return -32768;
    return (int16_t)f;
}

/** 把 UI (|magnitude|, dir) 合成有符号 int16 协议值 */
static int16_t signed_from_dir(float magnitude, jog_dir_t dir)
{
    if(magnitude < 0) magnitude = -magnitude;
    int16_t v = to_int16_clamp(magnitude);
    return (dir == JOG_DIR_NEG) ? (int16_t)(-v) : v;
}

/** 将已组好的协议帧字节流写入串口（带 TX 锁与字节统计） */
static int serial_write_frame(const uint8_t *bytes, int n)
{
    if(s_fd < 0 || !bytes || n <= 0) {
        return -1;
    }
    TX_LOCK();
    ssize_t w = write(s_fd, bytes, (size_t)n);
    TX_UNLOCK();
    if(w > 0) {
        STATE_LOCK();
        s_status.tx_bytes += (uint32_t)w;
        STATE_UNLOCK();
    }
    return (w == (ssize_t)n) ? 0 : -1;
}

/** 便利宏：pack→write 一气呵成（packer 为 mcu_pack_* 函数，可传可变参数） */
#define SERIAL_SEND_PACK(packer, ...) do {                              \
    uint8_t _b[MCU_FRAME_MAX_LEN];                                      \
    int _n = (packer)(_b, sizeof(_b), ##__VA_ARGS__);                   \
    serial_write_frame(_b, _n);                                         \
} while(0)

/* ============================================================================
 *  生命周期
 * ============================================================================ */
int serial_backend_init(const char *device, int baud)
{
    if(s_fd >= 0) {
        serial_backend_deinit();
    }
    if(!device || !device[0]) device = SERIAL_DEFAULT_DEVICE;
    if(baud <= 0)             baud   = SERIAL_DEFAULT_BAUD;

    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0) {
        fprintf(stderr, "[serial_backend] open %s failed: %s\n",
                device, strerror(errno));
        return -1;
    }
    if(configure_port(fd, baud) != 0) {
        fprintf(stderr, "[serial_backend] configure %s failed\n", device);
        close(fd);
        return -1;
    }

    strncpy(s_device, device, sizeof(s_device) - 1);
    s_device[sizeof(s_device) - 1] = '\0';
    s_baud = baud;
    s_fd   = fd;

    STATE_LOCK();
    memset(&s_status, 0, sizeof(s_status));
    s_status.bucket1_state = -1;
    s_status.bucket2_state = -1;
    s_status.door_closed   = true;   /* 默认假设门已关闭（安全态，UI 显示灰色） */
    STATE_UNLOCK();

    mcu_proto_reset_rx();            /* 清掉上次会话残留的半帧状态 */

    s_rx_running = true;
    if(pthread_create(&s_rx_thread, NULL, rx_thread_fn, NULL) != 0) {
        s_rx_running = false;
        close(s_fd);
        s_fd = -1;
        return -1;
    }
    fprintf(stderr, "[serial_backend] %s @ %d opened\n", s_device, s_baud);
    return 0;
}

void serial_backend_deinit(void)
{
    s_rx_running = false;
    if(s_rx_thread) {
        pthread_join(s_rx_thread, NULL);
        s_rx_thread = 0;
    }
    if(s_fd >= 0) {
        close(s_fd);
        s_fd = -1;
    }
}

bool serial_backend_is_open(void)        { return s_fd >= 0; }
const char *serial_backend_device(void)  { return s_device[0] ? s_device : SERIAL_DEFAULT_DEVICE; }
int  serial_backend_baud(void)           { return s_baud; }

void serial_backend_get_status(serial_status_t *out)
{
    if(!out) return;
    STATE_LOCK();
    *out = s_status;
    STATE_UNLOCK();
}

/* ============================================================================
 *  TX 命令实现
 *  ----------------------------------------------------------------------------
 *  所有 setter 走 SERIAL_SEND_PACK(pack_*)：mcu_protocol 组帧 → write。
 *  对 MCU 不会主动上报状态的开关（Laser/Gas/Fan/Light/Door 等）做乐观更新，
 *  避免 refresh_cb → sync_switch 把用户操作刷回默认。RX 接入后可在解析里覆盖。
 * ============================================================================ */
#define STATUS_SET(field, val) do { STATE_LOCK(); s_status.field = (val); STATE_UNLOCK(); } while(0)

/* ---- Motor ---- */
void serial_motor_jog(jog_axis_t axis, jog_dir_t dir, float speed)
{
    /* 单次 jog —— 等价于 start，调用方需自行配对 stop */
    serial_motor_jog_start(axis, dir, speed);
}

void serial_motor_jog_start(jog_axis_t axis, jog_dir_t dir, float speed)
{
    int16_t v = signed_from_dir(speed, dir);
    switch(axis) {
        case JOG_AXIS_Z: SERIAL_SEND_PACK(mcu_pack_z_jog_speed, v); break;
        case JOG_AXIS_S: SERIAL_SEND_PACK(mcu_pack_s_jog_speed, v); break;
        case JOG_AXIS_D: SERIAL_SEND_PACK(mcu_pack_d_jog_speed, v); break;
    }
}

void serial_motor_jog_stop(jog_axis_t axis)
{
    switch(axis) {
        case JOG_AXIS_Z: SERIAL_SEND_PACK(mcu_pack_z_jog_stop); break;
        case JOG_AXIS_S: SERIAL_SEND_PACK(mcu_pack_s_jog_stop); break;
        case JOG_AXIS_D: SERIAL_SEND_PACK(mcu_pack_d_jog_stop); break;
    }
}

void serial_motor_home(jog_axis_t axis)
{
    switch(axis) {
        case JOG_AXIS_Z: SERIAL_SEND_PACK(mcu_pack_z_home); break;
        case JOG_AXIS_S: SERIAL_SEND_PACK(mcu_pack_s_home); break;
        case JOG_AXIS_D: /* D 轴无 Home */ break;
    }
}

void serial_motor_increment(jog_axis_t axis, jog_dir_t dir, float distance)
{
    int16_t v = signed_from_dir(distance, dir);
    switch(axis) {
        case JOG_AXIS_Z: SERIAL_SEND_PACK(mcu_pack_z_inc, v); break;
        case JOG_AXIS_S: SERIAL_SEND_PACK(mcu_pack_s_inc, v); break;
        case JOG_AXIS_D: SERIAL_SEND_PACK(mcu_pack_d_inc, v); break;
    }
}

/* ---- Print ---- */
void serial_print_set_z_work_zero(void)
{
    SERIAL_SEND_PACK(mcu_pack_z_set_zero);
}

/** block=true → 三通堵粉(Block); block=false → 三通过粉(Release) */
void serial_print_three_way(bool block)
{
    if(block) SERIAL_SEND_PACK(mcu_pack_three_way_close);
    else      SERIAL_SEND_PACK(mcu_pack_three_way_open);
}

/** 回粉电机：协议未指派，保留 stub。dir: +1/-1/0 */
void serial_print_powder_motor(int dir)
{
    (void)dir;
    /* TODO: 协议未指派；后续在 mcu_protocol.* 里添加 mcu_pack_powder_motor() */
}

void serial_print_laser_set(bool on)
{
    if(on) SERIAL_SEND_PACK(mcu_pack_laser_enable);
    else   SERIAL_SEND_PACK(mcu_pack_laser_disable);
    STATUS_SET(laser_on, on);
}

void serial_print_wash_gas_set(bool on)
{
    if(on) SERIAL_SEND_PACK(mcu_pack_gas_on);
    else   SERIAL_SEND_PACK(mcu_pack_gas_off);
    STATUS_SET(wash_gas_on, on);
}

void serial_print_fan_set(bool on)
{
    if(on) SERIAL_SEND_PACK(mcu_pack_fan_on);
    else   SERIAL_SEND_PACK(mcu_pack_fan_off);
    STATUS_SET(fan_running, on);
}

/** 风机 PWM 占空比：固件硬约束 [35,100]，UI 输 0~100 时下界 clamp 到 35 */
void serial_print_fan_duty(uint8_t pct)
{
    if(pct > 100) pct = 100;
    if(pct < 35)  pct = 35;
    SERIAL_SEND_PACK(mcu_pack_fan_duty, pct);
    STATUS_SET(fan_duty_pct, pct);
}

/* ---- Settings ---- */
void serial_settings_bucket_set(int idx, bool open)
{
    if(idx < 1 || idx > 2) return;
    if(open) SERIAL_SEND_PACK(mcu_pack_bucket_on,  (uint8_t)idx);
    else     SERIAL_SEND_PACK(mcu_pack_bucket_off, (uint8_t)idx);
    STATE_LOCK();
    if(idx == 1) s_status.bucket1_state = (int8_t)(open ? 1 : 0);
    else         s_status.bucket2_state = (int8_t)(open ? 1 : 0);
    STATE_UNLOCK();
}

/* ---- 功能按键 ---- */
void serial_func_wake(void)
{
    SERIAL_SEND_PACK(mcu_pack_wake_active);
    STATUS_SET(awake, true);
}

void serial_func_sleep(void)
{
    SERIAL_SEND_PACK(mcu_pack_wake_inactive);
    STATUS_SET(awake, false);
}

void serial_func_enable(void)
{
    SERIAL_SEND_PACK(mcu_pack_motor_enable);
    /* enabled 字段以 MCU 上报的 TLV_MOTOR_ENABLED/DISENABLED 为准，
     * 不在此处乐观更新（避免与真实电机状态打架） */
}

void serial_func_disable(void)
{
    SERIAL_SEND_PACK(mcu_pack_motor_disable);
}

void serial_func_light_set(bool on)
{
    if(on) SERIAL_SEND_PACK(mcu_pack_light_on);
    else   SERIAL_SEND_PACK(mcu_pack_light_off);
    STATUS_SET(light_on, on);
}

/** 主动查询门状态：协议无对应接口，保留 no-op */
void serial_func_door_query(void)
{
    /* TODO: 协议无主动查询接口；门状态依赖 MCU 自身上报 */
}

void serial_func_door_set(bool open)
{
    if(open) SERIAL_SEND_PACK(mcu_pack_door_open);
    else     SERIAL_SEND_PACK(mcu_pack_door_close);
    /* door_closed = !open（UI 开关 ON=open；status 字段语义保持原样） */
    STATUS_SET(door_closed, !open);
}
