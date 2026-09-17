[TOC]

# 功能指令

## 1. System OTA 全量包升级

### 1.1 网络远程 OTA 服务器升级

```bash
# updateEngine --update --image_url=固件地址 --partition=0x3FFC00 --version_url=版
本⽂件地址 --savepath=保存的固件地址 --reboot
updateEngine --image_url=http://172.16.21.110:8080/linuxab/update_ota.img --
update --reboot
```

### 1.2 本地 升级

```bash
# updateEngine --update --image_url=固件地址(update_ab.img 或 update_ota.img) --
partition=0x3FFC00 --version_url=版本⽂件地址 --savepath=保存的固件地址 --reboot
updateEngine --image_url=/userdata/update_ota.img --update --reboot
```

### 升级指令参数和流程

```bash
流程介绍：
1. 固件版本⽐较
2. 下载固件(--image_url)，并保存到本地(--savepath)
3. 升级指定的分区(--partition)
4. 设置升级分区为将要引导分区
5. 重启
6. 尝试引导升级的分区
可缺省参数：
1. --partition：设置将要升级的分区 ，Linux A/B模式下，建议只升级uboot_a/uboot_b、
boot_a/boot_b和system_a/system_b，即0xFC00，不⽀持升级parameter 和loader分区。详⻅
参数说明
2. --version：没有设置该参数，则不会进⾏版本⽐较
3. --savepath：固件保存地址，缺省时为/tmp/update.img，建议使⽤默认值
4. --reboot：升级完后重启
```



# 外设、传感器控制指令

## 1. 六路 led 和 12V pm2.5 电磁阀 开关控制

```bash
# 六路 led 控制节点路径 /sys/class/leds 
root@rk3568-buildroot:/sys/class/leds# ls
fa_valve  lgt1l  lgt1r  lgt2l  lgt2r  lgt3l  lgt3r
# 以左侧 1 灯 lgt1l  举列子 
root@rk3568-buildroot:/sys/class/leds/lgt1l# ls
brightness  device  max_brightness  power  subsystem  trigger  uevent

# 向 /sys/class/leds/lgt1l/brightness 文件 wirte 1 为打开； write 0 为关闭 （应该会有单独控制需求）
root@rk3568-buildroot:/sys/class/leds/lgt1l# cat brightness
0
root@rk3568-buildroot:/sys/class/leds/lgt1l# echo 1 > brightness
root@rk3568-buildroot:/sys/class/leds/lgt1l# cat brightness
1
root@rk3568-buildroot:/sys/class/leds/lgt1l#

###  fa_valve  节点为 12V 电磁阀 开关 pm2.5 的盒子

root@rk3568-buildroot:/sys/class/leds# cd fa_valve
root@rk3568-buildroot:/sys/class/leds/fa_valve# ls
brightness  device  max_brightness  power  subsystem  trigger  uevent
root@rk3568-buildroot:/sys/class/leds/fa_valve# cat brightness
0
root@rk3568-buildroot:/sys/class/leds/fa_valve# echo 1 > brightness
root@rk3568-buildroot:/sys/class/leds/fa_valve# cat brightness
1
root@rk3568-buildroot:/sys/class/leds/fa_valve#
```

## 2. speaker 通路配置和播放

```bash
aplay -l                                        # 确认序号，name 是 rockchip-rk809
amixer -c 0 cset name='Playback Path' SPK
gst-launch-1.0 audiotestsrc freq=440 ! audioconvert ! audioresample ! alsasink device=hw:0,0
aplay -D hw:0,0 music.wav
```

## 3. 以太网测试

```bash
ls -l /sys/class/net/eth1/phydev/driver    # 期望指向 "YT8531 Gigabit Ethernet"
cat /sys/class/net/eth1/phydev/phy_id      # 期望 0x4f51e91b
dmesg | grep -iE "eth1|mdio|stmmac"
```

## 4. 屏幕背光调节

| DT 节点      | alias | 用途               | 控制节点位置            |
| ------------ | ----- | ------------------ | ----------------------- |
| pwm@fdd70020 | pwm2  | O2_PWM（GPIO0_C1） | /sys/class/pwm/pwmchip0 |
| pwm@fe6e0010 | pwm5  | LCD 背光           | /sys/class/pwm/pwmchip1 |

```bash

# 由暗到明范围  0 ~ 255 
echo  255  > /sys/class/backlight/backlight/brightness 
```

## 5. usb test

```bash
ls /sys/class/udc/              # 应稳定显示 fcc00000.usb
dmesg | grep -c "read descriptors"   # 应停止增长
dmesg | grep xhci-hcd.0         # 应该没有了，只剩 xhci-hcd.1.auto
```

## 6. 氧浓度查看及换算

```bash
# 读 ADC 原始值（氧浓度换算用 k 系数在用户态做）
cat /sys/bus/iio/devices/iio:device0/in_voltage0_raw    电压值 需要转换公式计算

cd /sys/class/pwm/pwmchip0   # 对应 pwm0   pwm@fdd70020

# 添加脚本预热 100s
/etc/init.d/S46o2-preheat start
cat /sys/class/pwm/pwmchip0/pwm0/duty_cycle   # 应该在往上爬
ls /run/o2_ready                              # 约 100s 后出现

```

## 7. spi 小屏测试

```bash
# 1. 看背光节点在不在
ls /sys/class/backlight/
# 一般是 spi-lcd-backlight 或类似名字

# 2. 强制开背光
for d in /sys/class/backlight/*; do
  echo 0 > "$d/bl_power"
  echo 1 > "$d/brightness"
  echo "$d: power=$(cat $d/bl_power) bri=$(cat $d/brightness)"
done

# 3. 直接顶 GPIO2_D5 (bank2 的 bit13)
# 先确认 gpiochip
ls /sys/class/gpio/
cat /sys/kernel/debug/gpio | head -80

dmesg | grep -i backlight
# 量一下 J11 的 3V3_LCD、LCD_LEDA 是否有 3.3V

# 1. 确认设备
ls -l /dev/dri/
cat /sys/kernel/debug/dri/0/name
st7735r dev=spi2.1
# 2. 查询连接器和显示模式
modetest -M st7735r -c

modetest -M st7735r -c
Connectors:
id      encoder status          name            size (mm)       modes   encoders
31      0       connected       SPI-1           11x22           1       35
  modes:
        index name refresh (Hz) hdisp hss hse htot vdisp vss vse vtot
  #0 80x160 0.08 80 80 80 80 160 160 160 160 1 flags: ; type: preferred, driver
  props:
        1 EDID:
                flags: immutable blob
                blobs:

                value:
        2 DPMS:
                flags: enum
                enums: On=0 Standby=1 Suspend=2 Off=3
                value: 0
        5 link-status:
                flags: enum
                enums: Good=0 Bad=1
                value: 0
        6 non-desktop:
                flags: immutable range
                values: 0 1
                value: 0
        4 TILE:
                flags: immutable blob
                blobs:

                value:


# 3. 测试
modetest -M st7735r -s 31@34:80x160

# 如果提示找不到 CRTC，再查一次：

modetest -M st7735r -p
# 看 CRTCs: 里的 id，例如是 32，就改成：

modetest -M st7735r -s 31@32:80x160
# 成功后小屏会刷出彩色测试图案。按 Ctrl+C 退出。

# 跑 modetest 前后各看一次，计数应该有变化 测试是否有输出
cat /proc/interrupts | grep -i spi


# A. 确认 phandle 是否存在（决定上面走哪个分支）
ls /proc/device-tree/spi-lcd-backlight/
# B. 正确的开背光姿势：先 bl_power 再 brightness
B=/sys/class/backlight/spi-lcd-backlight
echo 0 > $B/bl_power
echo 1 > $B/brightness
cat $B/bl_power $B/brightness
cat /sys/kernel/debug/gpio | grep backlight
# C. 背光确认亮了之后，开 drm 调试发命令
echo 0x02 > /sys/module/drm/parameters/debug
dmesg -C
D=/sys/kernel/debug/dri/0/command
echo 01 > $D; sleep 1
echo 11 > $D; sleep 1
echo 29 > $D
echo 21 > $D
dmesg | tail -40
```

## 8. spi 灯效板 控制

```bash
ls /dev/spidev1.0                              # 设备节点应该出现
cat /sys/kernel/debug/clk/clk_spi1/clk_rate    # 应该是 200000000
```

### 用户态 控制代码

```c
// 彩虹流水、单色流水拖尾、彩虹呼吸那三个 Tick 函数
// 每颗灯 24bit × 4 = 96 SPI 位 = 12 字节，而且正好字节对齐（1 字节 = 2 个 WS 位，高低半字节各一个，编码时不用跨字节移位，比 MCU 那版 3 位编码还简单）。40 颗 = 480 字节，加 100 字节复位尾共 580 字节，远低于 ROCKCHIP_SPI_MAX_TRANLEN（65535）和 spidev 默认 bufsiz（4096）。

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#define LED_NUM        40
#define BYTES_PER_LED  12          /* 24bit x 4 SPI位 / 8 */
#define RESET_BYTES    100         /* 100 x 8 x 320ns = 256us > 200us */
#define BUF_SIZE       (LED_NUM * BYTES_PER_LED + RESET_BYTES)
#define WS_SPEED_HZ    3200000
static uint8_t buf[BUF_SIZE];
static int fd = -1;
/* WS 位 0 -> 0b1000, 1 -> 0b1110 */
static const uint8_t nib[2] = { 0x8, 0xE };
static void enc_byte(uint32_t *p, uint8_t v)
{
    for (int i = 0; i < 8; i += 2) {
        uint8_t hi = nib[(v >> (7 - i)) & 1];
        uint8_t lo = nib[(v >> (6 - i)) & 1];
        buf[(*p)++] = (uint8_t)((hi << 4) | lo);
    }
}
void ws_set_color(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t p;
    if (idx < 0 || idx >= LED_NUM) return;
    p = (uint32_t)idx * BYTES_PER_LED;
    enc_byte(&p, g);               /* GRB 顺序、高位先发 */
    enc_byte(&p, r);
    enc_byte(&p, b);
}
int ws_open(void)
{
    uint8_t mode = SPI_MODE_0, bits = 8;
    uint32_t hz = WS_SPEED_HZ;
    fd = open("/dev/spidev1.0", O_RDWR);
    if (fd < 0) return -1;
    ioctl(fd, SPI_IOC_WR_MODE, &mode);
    ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &hz);
    memset(buf, 0, sizeof(buf));   /* 数据区清零 + 帧尾复位区恒为 0 */
    return 0;
}
void ws_send(void)
{
    struct spi_ioc_transfer tr = {
        .tx_buf        = (unsigned long)buf,
        .rx_buf        = 0,
        .len           = BUF_SIZE,
        .speed_hz      = WS_SPEED_HZ,
        .bits_per_word = 8,
    };
    ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
}
```

## 9. i2c 转接板

```bash
# 都挂在 0x70 mux 地址按 A2/A1/A0 全接地取的 0x70，通道分配按"ch0 温湿度 + ch1~3 气压"写的
# 这两处都要对传感器板原理图核一下，不一致的话改 dtsi 里的 reg 就行。另外上电后如果 i2cdetect -y 2 连 0x70 都扫不到，先查主机段的上拉：SoC 侧 		pcfg_pull_none_smt 不带内部上拉，上拉全在传感器板上。

i2c10 = "/i2c@fe5b0000/i2c-mux@70/i2c@0";   # humidity@38 aosong,aht20
i2c11 = "/i2c@fe5b0000/i2c-mux@70/i2c@1";
i2c12 = "/i2c@fe5b0000/i2c-mux@70/i2c@2";
i2c13 = "/i2c@fe5b0000/i2c-mux@70/i2c@3";
```

### 	9.1 AHT20 温湿度传感器 数据读取

```shell
i2cdetect -l            # 应出现 i2c-10..13, 类型 "i2c-2-mux (chan_id 0..3)"
i2cdetect -y 2          # 只有 0x70
cat /sys/class/hwmon/hwmon*/name            # 其中一个是 aht20
cat /sys/class/hwmon/hwmonX/temp1_input     # 毫摄氏度
cat /sys/class/hwmon/hwmonX/humidity1_input # 毫百分比

# CPM121: MR -> 等 >=5ms -> GD
i2ctransfer -y 11 w2@0x6c 0x30 0x0a && sleep 0.01 && i2ctransfer -y 11 w1@0x6c 0x06 r5


i2cdetect -l                  # 应该看到 i2c-10..13 是 "i2c-2-mux (chan_id N)"
i2cdetect -y 2                # 只应看到 0x70 (mux 本体)
i2cdetect -y 10               # 0x38
i2cdetect -y 11               # 0x6C
cat /sys/class/hwmon/hwmon*/name

/sys/class/hwmon/hwmonX/temp1_input   # 毫摄氏度
/sys/class/hwmon/hwmonX/humidity1_input

```

### 9.2  CPM 121 压力传感器

```c
// 时序很简单（手册 3.2~3.3）：
/*
MR：向 0x6C 写两字节 0x30 0x0A，STOP
等 ≥5ms（一次完整转换约 5ms）
GD：写寄存器地址 0x06，重复起始后读 5 字节：3 字节压力 + 2 字节温度，大端
换算：P[kPa] = (P3<<16 | P2<<8 | P1) / 64 / 1000，T[°C] = (int16)(TH<<8 | TL) / 256
用户态版本（配合上面的 alias，三个传感器分别是 /dev/i2c-11/12/13，不需要自己切通道，内核 mux 框架会在每次 transfer 前自动选通并加锁）：
*/

int fd = open("/dev/i2c-11", O_RDWR);
ioctl(fd, I2C_SLAVE, 0x6c);
u8 mr[2] = { 0x30, 0x0a };
write(fd, mr, 2);
usleep(6000);
u8 reg = 0x06, buf[5];
struct i2c_msg msgs[2] = {
	{ .addr = 0x6c, .flags = 0,        .len = 1, .buf = &reg },
	{ .addr = 0x6c, .flags = I2C_M_RD, .len = 5, .buf = buf  },
};
struct i2c_rdwr_ioctl_data d = { .msgs = msgs, .nmsgs = 2 };
ioctl(fd, I2C_RDWR, &d);
u32 raw = (buf[0] << 16) | (buf[1] << 8) | buf[2];
double kpa = raw / 64.0 / 1000.0;
double degc = (s16)((buf[3] << 8) | buf[4]) / 256.0;


如果想做成内核驱动，写个 drivers/iio/pressure/cpm121.c，IIO_PRESSURE + IIO_TEMP 两个通道，read_raw 里做上面这套 MR→延时→GD，scale 用 1/64000（IIO 压力单位是 kPa，正好）。这样三个传感器会变成三个 iio:deviceX，比用户态记设备号清楚，也能和你们已有的 hx711（avia,hx711）保持一致的风格。DT 我上面已经按 consensic,cpm121 写好了，驱动没写之前这三个节点只是不 probe，不影响启动。
```

## 10. 霍尔检测

```bash
cat /run/hall/door_hall      # 0 = 门已关, 1 = 门开
cat /run/hall/filter_hall    # 0 = 滤网在位, 1 = 滤网缺失
```

## 11. 急停

```bash
# gpio-key 模式 KEY_STOP
evtest 
```

