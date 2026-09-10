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
 * SOC 全包升级不走 AB 对规则：包内唯一 .img 解包后统一改名为 UPC_SOC_OTA_IMG，
 * 固定单文件避免多次升级累积；updateEngine 的 --image_url 直接取该路径。
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

#endif /* UPC_IMAGE_PATHS_HPP */
