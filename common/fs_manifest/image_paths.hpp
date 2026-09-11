#ifndef UPC_IMAGE_PATHS_HPP
#define UPC_IMAGE_PATHS_HPP

/**
 * OTA 固件镜像落盘根路径（SOC / MCU / Galvo）。
 *
 *   /userdata/image/
 *     tmp/     云端下载暂存
 *     soc/     保留：SOC 全包升级已改为落到 UPC_SOC_OTA_IMG，不再用此目录
 *     mcu/     串口 MCU OTA（MainBoard_Slot{A|B}_V*.bin，见 OTA_IMAGE.md）
 *     galvo/   以太网振镜 OTA（Galvo_Slot{A|B}_V*.bin，见 OTA_IMAGE.md）
 *
 * MQTT 下载与 Glavo 刷写共用此头，避免两处硬编码分叉。
 * AB 对选取：`ota_image_pair.*` / [OTA_IMAGE.md](OTA_IMAGE.md)。
 *
 * SOC 有两种升级形态，都不走 AB 对规则：
 *   全包：包内唯一 .img 解包后统一改名为 UPC_SOC_OTA_IMG，固定单文件避免多次
 *         升级累积；updateEngine 的 --image_url 直接取该路径。
 *   应用层：包内是一组 rongsu_* 可执行文件，落到 UPC_APP_SLOT_ROOT 下的非活动
 *         槽（APP1/APP2 轮换），切槽后重启生效，跑不稳由启动脚本自动回滚。
 */

#ifndef UPC_IMAGE_ROOT
#define UPC_IMAGE_ROOT  "/userdata/image"
#endif

#ifndef UPC_IMAGE_TMP_DIR
#define UPC_IMAGE_TMP_DIR  UPC_IMAGE_ROOT "/tmp"
#endif

#ifndef UPC_IMAGE_SOC_DIR
#define UPC_IMAGE_SOC_DIR  UPC_IMAGE_ROOT "/soc"
#endif

#ifndef UPC_IMAGE_MCU_DIR
#define UPC_IMAGE_MCU_DIR  UPC_IMAGE_ROOT "/mcu"
#endif

#ifndef UPC_IMAGE_GALVO_DIR
#define UPC_IMAGE_GALVO_DIR  UPC_IMAGE_ROOT "/galvo"
#endif

/** SOC 全包升级固件的固定落盘路径（updateEngine --image_url 取此值） */
#ifndef UPC_SOC_OTA_IMG
#define UPC_SOC_OTA_IMG  "/userdata/update_ota.img"
#endif

/** SOC 待应用固件的状态文件（与设备密钥 / MQTT 凭据同目录，一并活过 OTA） */
#ifndef UPC_SOC_OTA_PENDING_FILE
#define UPC_SOC_OTA_PENDING_FILE  "/userdata/snowpod/soc-ota-pending"
#endif

/**
 * 应用层 A/B 槽：两个槽各存一整套 rongsu_* 与配套的 rongsu_app.sh。
 *
 * 槽内只放随版本走的可执行文件；snowslice、相机 IQ、rknn 几何契约等共享资源
 * 仍留在 UPC_APP_SLOT_ROOT 顶层，不参与轮换（体积大且与业务版本弱耦合）。
 */
#ifndef UPC_APP_SLOT_ROOT
#define UPC_APP_SLOT_ROOT  "/home"
#endif

#ifndef UPC_APP_SLOT_A
#define UPC_APP_SLOT_A  "APP1"
#endif

#ifndef UPC_APP_SLOT_B
#define UPC_APP_SLOT_B  "APP2"
#endif

/** 槽状态文件：active/prev/trial/boot_count/rollback，由启动脚本与 MQTT 共读 */
#ifndef UPC_APP_SLOT_STATE_FILE
#define UPC_APP_SLOT_STATE_FILE  UPC_APP_SLOT_ROOT "/app_slot"
#endif

/** 薄 bootstrap：选槽 / 回滚 / 交棒给槽内 rongsu_app.sh，并提供 slot-* 子命令 */
#ifndef UPC_APP_BOOTSTRAP
#define UPC_APP_BOOTSTRAP  UPC_APP_SLOT_ROOT "/rongsu.sh"
#endif

#endif /* UPC_IMAGE_PATHS_HPP */
