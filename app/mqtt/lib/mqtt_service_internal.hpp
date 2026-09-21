#ifndef UPC_MQTT_SERVICE_INTERNAL_HPP
#define UPC_MQTT_SERVICE_INTERNAL_HPP

#include "image_paths.hpp"
#include "rs_log.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace upc::mqtt {

/** MQTT 连接/遥测轮询相关常量。 */
inline constexpr uint64_t kReconnectIntervalMs = 3000;
inline constexpr const char* kRotateCredentialAction = "ROTATE_CREDENTIAL";
inline constexpr uint64_t kTelemetryPrintingIntervalMs = 2000;
inline constexpr uint64_t kTelemetryIdleIntervalMs = 30000;
/** V4 舱外 PM 遥测间隔（协议 chamber_ext 5–10s） */
inline constexpr uint64_t kTelemetryChamberExtIntervalMs = 8000;
inline constexpr uint64_t kFirmwareQueryIntervalMs = 10000;

/** 云控命令协议关键字与超时配置。 */
inline constexpr const char* kCmdSetLight = "set_light";
inline constexpr const char* kCmdStartRtspPush = "start_rtsp_push";
inline constexpr const char* kCmdStopRtspPush = "stop_rtsp_push";
inline constexpr const char* kCmdLogCollect = "log_collect";
inline constexpr const char* kCmdRejectReasonInvalidState = "invalid_state";
inline constexpr const char* kCmdRejectReasonEncoderError = "encoder_error";
inline constexpr uint64_t kRtspAckTimeoutMs = 8000;

/**
 * 日志采集任务级 error.reason 闭集（设备端不自造码）。
 * maxPacketSize 判超限已屏蔽，SIZE_EXCEEDED / size_exceeded 当前不会再产生，
 * 保留定义以维持闭集完整、便于日后恢复限制。
 */
inline constexpr const char* kLogErrUrlExpired = "URL_EXPIRED";
inline constexpr const char* kLogErrPutFailed = "PUT_FAILED";
inline constexpr const char* kLogErrDiskFull = "DISK_FULL";
inline constexpr const char* kLogErrEncryptFail = "ENCRYPT_FAIL";
inline constexpr const char* kLogErrNoFileFound = "NO_FILE_FOUND";
inline constexpr const char* kLogErrSizeExceeded = "SIZE_EXCEEDED";
inline constexpr const char* kLogErrInternal = "INTERNAL";

/** fileReport.reason 闭集（collected=false 时）。 */
inline constexpr const char* kLogFileNotFound = "not_found";
inline constexpr const char* kLogFileEmpty = "empty";
inline constexpr const char* kLogFileSizeExceeded = "size_exceeded";

/** 日志采集加密与落盘约定。 */
inline constexpr const char* kLogEncryptAlgorithm = "AES-256-GCM";
inline constexpr const char* kLogHkdfInfo = "snowx:iot:log-collect:enc:v1";
inline constexpr const char* kLogPutContentType = "application/octet-stream";
inline constexpr const char* kTopicLogDir = UPC_TOPIC_LOG_DIR;
inline constexpr const char* kLogCollectTmpDir = "/tmp/snowpod_log_collect";
inline constexpr int kLogRotateMaxGenerations = 3;
inline constexpr size_t kLogGcmNonceLen = 12;
inline constexpr size_t kLogGcmTagLen = 16;
inline constexpr size_t kLogAesKeyLen = 32;

/** OTA 包下载/落盘目录约定（根路径见 common/fs_manifest/image_paths.hpp）。 */
inline constexpr const char* kImageTmpDir = UPC_IMAGE_TMP_DIR;
inline constexpr const char* kImageSocDir = UPC_IMAGE_SOC_DIR;
inline constexpr const char* kImageMcuDir = UPC_IMAGE_MCU_DIR;
inline constexpr const char* kImageGalvoDir = UPC_IMAGE_GALVO_DIR;

/** SOC 全包升级：固件固定路径、待应用状态文件、刷写工具。 */
inline constexpr const char* kSocOtaImgPath = UPC_SOC_OTA_IMG;
inline constexpr const char* kSocOtaPendingFile = UPC_SOC_OTA_PENDING_FILE;
inline constexpr const char* kSocOtaUpdateEngine = "updateEngine";
/** 解包后除 .img 自身外还要留的余量，避免把 /userdata 刚好占满。 */
inline constexpr uint64_t kSocOtaSpareBytes = 64ull * 1024 * 1024;

/** SOC 应用层 A/B 升级：槽根目录、槽名、状态文件、bootstrap 入口。 */
inline constexpr const char* kAppSlotRoot = UPC_APP_SLOT_ROOT;
inline constexpr const char* kAppSlotA = UPC_APP_SLOT_A;
inline constexpr const char* kAppSlotB = UPC_APP_SLOT_B;
inline constexpr const char* kAppSlotStateFile = UPC_APP_SLOT_STATE_FILE;
inline constexpr const char* kAppBootstrap = UPC_APP_BOOTSTRAP;
/** 槽内启动脚本，随可执行文件一起升级（进程列表变更不影响回滚）。 */
inline constexpr const char* kAppSlotScriptName = "rongsu_app.sh";
/** 包内声明升级方式的描述文件；缺失时退回按解包内容推断。 */
inline constexpr const char* kSocOtaManifestName = "ota_manifest.json";
/** 落盘应用层固件时槽根分区要留的余量。 */
inline constexpr uint64_t kAppSlotSpareBytes = 8ull * 1024 * 1024;

/** 最小 URL 结构，供 OTA 下载链路使用。 */
struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string path;
    uint16_t port = 0;
};

/** MQTT 原始帧调试输出（受编译开关控制）。 */
void debug_log_mqtt_frame(const char* direction,
                          const std::string& topic,
                          const void* payload,
                          size_t payload_len,
                          int qos,
                          int retained);
/** 解析 http/https URL。 */
bool parse_url(const std::string& raw, ParsedUrl& out);
/** 从 URL path 提取下载文件名。 */
std::string basename_from_url_path(const std::string& path);
/** 目录不存在则创建。 */
bool ensure_dir(const std::filesystem::path& p);
/** 清空 OTA 下载暂存目录，回收掉电中断留下的残留。 */
void clear_image_tmp_dir();
/** 计算文件 sha256（十六进制小写）。 */
std::string hex_sha256_file(const std::filesystem::path& p);
/** 下载文件并在接收时回调进度。 */
bool download_file_with_progress(const std::string& url,
                                 const std::filesystem::path& out_file,
                                 const std::function<void(size_t, size_t)>& on_progress,
                                 std::string& err);
/**
 * HTTPS PUT 预签名直传：body 原样写入，固定 Content-Type: application/octet-stream
 * （与云端 Oss/S3 预签名头一致）。成功返回 true。
 */
bool upload_http_put(const std::string& url,
                     const void* body,
                     size_t body_len,
                     std::string& err,
                     const std::function<void(size_t, size_t)>& on_progress = {});
/** OTA 组件名映射到目标目录。 */
const char* component_target_dir(const std::string& component);
/** shell 安全单引号转义。 */
std::string sh_single_quote(const std::string& v);
/** 去掉行首尾空白和换行。 */
std::string trim_line(std::string s);
/** tar 条目安全校验（平铺、无路径穿越）。 */
bool archive_entry_safe_flat(const std::string& entry);
/** 执行命令并按行采集输出。 */
bool run_capture_lines(const std::string& cmd, std::vector<std::string>& lines);
/** 执行命令并返回是否成功。 */
bool run_shell_ok(const std::string& cmd);
/** ASCII 大写转换。 */
std::string to_upper_ascii(std::string s);
/** 是否为 .bin 文件名。 */
bool is_bin_name(const std::string& name);
/** 是否为 .img 文件名（SOC 全包升级固件）。 */
bool is_img_name(const std::string& name);
/** 查询 path 所在文件系统的可用字节数（非特权可用部分）。 */
bool fs_free_bytes(const char* path, uint64_t& out);
/** 设备激活失败重试退避。 */
uint64_t activation_retry_interval_ms(uint32_t fail_count);

} // namespace upc::mqtt

#endif
