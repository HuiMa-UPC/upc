#ifndef UPC_IMAGE_PATHS_HPP
#define UPC_IMAGE_PATHS_HPP

/**
 * OTA 固件镜像落盘根路径（SOC / MCU / Galvo）。
 *
 *   /data/image/
 *     tmp/     云端下载暂存
 *     soc/     SOC 升级包
 *     mcu/     串口 MCU OTA（MainBoard_Slot{A|B}_V*.bin，见 OTA_IMAGE.md）
 *     galvo/   以太网振镜 OTA（Galvo_Slot{A|B}_V*.bin，见 OTA_IMAGE.md）
 *
 * MQTT 下载与 Glavo 刷写共用此头，避免两处硬编码分叉。
 * AB 对选取：`ota_image_pair.*` / [OTA_IMAGE.md](OTA_IMAGE.md)。
 */

#ifndef UPC_IMAGE_ROOT
#define UPC_IMAGE_ROOT  "/data/image"
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

#endif /* UPC_IMAGE_PATHS_HPP */
