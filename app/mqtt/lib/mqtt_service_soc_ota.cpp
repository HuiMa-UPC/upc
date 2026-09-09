#include "mqtt_service.hpp"

#include "mqtt_service_internal.hpp"
#include "rs_log.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace upc::mqtt {

#ifndef UPC_SW_VERSION
#define UPC_SW_VERSION "unknown"
#endif

namespace {

/**
 * 宽容的固件版本比较：忽略大小写与前导 'V'。
 *
 * 云端 manifest 的 version 与本地 UPC_PRODUCT_VERSION 由两边各自维护，
 * "4.1.2" 与 "V4.1.2" 实际是同一版本，严格串比会把已升级判成未升级。
 */
bool same_firmware_version(const std::string& a, const std::string& b)
{
    auto normalize = [](const std::string& in) {
        std::string out = to_upper_ascii(in);
        if(!out.empty() && out.front() == 'V') {
            out.erase(0, 1);
        }
        return out;
    };
    const std::string na = normalize(a);
    return !na.empty() && na == normalize(b);
}

/** 按空白切分一行。 */
std::vector<std::string> split_ws(const std::string& line)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while(pos < line.size()) {
        while(pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        const size_t begin = pos;
        while(pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) {
            ++pos;
        }
        if(pos > begin) {
            out.push_back(line.substr(begin, pos - begin));
        }
    }
    return out;
}

bool all_digits(const std::string& s)
{
    if(s.empty()) {
        return false;
    }
    for(const char c : s) {
        if(!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

/**
 * 解析一行 `tar -tzvf` 输出，取出条目名与解包后字节数。
 *
 * GNU tar 与 busybox tar 的列宽不同，但字段顺序一致：
 *   mode owner/group size date time name
 * date 含 '-'、time 含 ':'，都不是纯数字，所以从右往左跳过 name 后
 * 第一个纯数字 token 就是 size。这样不依赖固定列位置。
 */
bool parse_tar_verbose_line(const std::string& line,
                            std::string& out_name,
                            uint64_t& out_size)
{
    const std::vector<std::string> tok = split_ws(line);
    if(tok.size() < 4) {
        return false;
    }
    out_name = tok.back();
    for(size_t i = tok.size() - 1; i-- > 0;) {
        if(all_digits(tok[i])) {
            out_size = std::strtoull(tok[i].c_str(), nullptr, 10);
            return true;
        }
    }
    return false;
}

} // namespace

/**
 * 校验包内唯一 .img 并落到固定路径，写下待应用状态。
 *
 * 与 mcu/galvo 的差别：不走 AB 对规则、不做整目录替换、也不在这里刷写。
 * 刷写时机交给用户，见 on_soc_ota_apply。
 */
bool MqttService::run_soc_ota_stage(const std::string& task_id,
                                    const OtaComponent& comp,
                                    const std::filesystem::path& tar_gz,
                                    middleware::OtaProgressPayload& progress,
                                    std::string& err_code)
{
    err_code.clear();
    std::error_code ec;

    /* 1) 列条目：SOC 包必须是平铺的单个 .img，同时取到解包后大小做空间预判。 */
    std::vector<std::string> lines;
    const std::string list_cmd = "tar -tzvf " + sh_single_quote(tar_gz.string());
    if(!run_capture_lines(list_cmd, lines)) {
        err_code = "tar_list_failed";
        return false;
    }

    std::string img_entry;
    uint64_t img_bytes = 0;
    size_t entry_count = 0;
    for(const std::string& raw : lines) {
        const std::string line = trim_line(raw);
        if(line.empty()) {
            continue;
        }
        std::string name;
        uint64_t size = 0;
        if(!parse_tar_verbose_line(line, name, size)) {
            err_code = "tar_list_failed";
            return false;
        }
        ++entry_count;
        if(!archive_entry_safe_flat(name) || !is_img_name(name)) {
            UPC_LOG_MQTT_SERVICE(Warn, "soc ota reject tar entry=%s", name.c_str());
            err_code = "tar_entry_invalid";
            return false;
        }
        img_entry = name;
        img_bytes = size;
    }
    if(entry_count == 0) {
        err_code = "img_missing";
        return false;
    }
    if(entry_count > 1) {
        err_code = "img_ambiguous";
        return false;
    }

    UPC_LOG_MQTT_SERVICE(Info, "soc ota entry=%s img_bytes=%llu",
                         img_entry.c_str(),
                         static_cast<unsigned long long>(img_bytes));

    /* 2) 先清掉上一次遗留的待应用固件：新包已下载完，旧的必然废弃，
     *    而且腾出的空间正好给本次解包用。 */
    soc_ota_pending_clear();
    std::filesystem::remove(kSocOtaImgPath, ec);

    /* 3) 空间预判：解包后要在同一文件系统上多放一份完整 .img。 */
    uint64_t free_bytes = 0;
    if(fs_free_bytes(UPC_IMAGE_ROOT, free_bytes)) {
        const uint64_t need = img_bytes + kSocOtaSpareBytes;
        if(img_bytes > 0 && free_bytes < need) {
            UPC_LOG_MQTT_SERVICE(Error,
                "soc ota no space: need=%llu free=%llu",
                static_cast<unsigned long long>(need),
                static_cast<unsigned long long>(free_bytes));
            err_code = "no_space";
            return false;
        }
    } else {
        UPC_LOG_MQTT_SERVICE(Warn, "soc ota statvfs failed, skip space check");
    }

    progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Applying);
    progress.progress_pct = 95;
    publish_ota_progress_ui(progress);
    (void)publish_ota_progress_cloud(progress);

    /* 4) 解包到 tmp 子目录而不是直接摊到 /userdata：与 kSocOtaImgPath 同一
     *    文件系统，后面 rename 是原子且零拷贝的，也不会污染 /userdata 顶层。 */
    const std::filesystem::path extract_dir =
        std::filesystem::path(kImageTmpDir) / (task_id + "_soc_extract");
    std::filesystem::remove_all(extract_dir, ec);
    if(!ensure_dir(extract_dir)) {
        err_code = "extract_dir_failed";
        return false;
    }

    const std::string extract_cmd =
        "tar -xzf " + sh_single_quote(tar_gz.string()) +
        " -C " + sh_single_quote(extract_dir.string());
    if(!run_shell_ok(extract_cmd)) {
        std::filesystem::remove_all(extract_dir, ec);
        err_code = "tar_extract_failed";
        return false;
    }

    /* 5) 用真实目录内容定位 .img，而不是复用 tar 输出里的名字：
     *    条目名可能含空白导致上面的列解析取不全，这里以文件系统为准。 */
    std::filesystem::path img_src;
    size_t img_found = 0;
    for(const auto& ent : std::filesystem::directory_iterator(extract_dir, ec)) {
        if(ec || !ent.is_regular_file()) {
            continue;
        }
        if(!is_img_name(ent.path().filename().string())) {
            continue;
        }
        img_src = ent.path();
        ++img_found;
    }
    if(img_found != 1) {
        std::filesystem::remove_all(extract_dir, ec);
        err_code = (img_found == 0) ? "img_missing" : "img_ambiguous";
        return false;
    }

    /* 6) 改名到固定路径：永远只留一份，多次升级不会累积几百 MB 的垃圾。 */
    std::filesystem::rename(img_src, kSocOtaImgPath, ec);
    if(ec) {
        UPC_LOG_MQTT_SERVICE(Error, "soc ota rename to %s failed: %s",
                             kSocOtaImgPath, ec.message().c_str());
        std::filesystem::remove_all(extract_dir, ec);
        err_code = "img_install_failed";
        return false;
    }
    std::filesystem::remove_all(extract_dir, ec);

    const uint64_t final_bytes =
        static_cast<uint64_t>(std::filesystem::file_size(kSocOtaImgPath, ec));

    /* 7) 记下待应用状态：用户点"稍后"之后可能断电，重启还要能找回入口。 */
    SocOtaPending pending{};
    pending.task_id = task_id;
    pending.version = comp.version;
    pending.img_path = kSocOtaImgPath;
    pending.img_bytes = ec ? img_bytes : final_bytes;
    if(!soc_ota_pending_save(pending)) {
        std::filesystem::remove(kSocOtaImgPath, ec);
        err_code = "pending_save_failed";
        return false;
    }

    UPC_LOG_MQTT_SERVICE(Info,
        "soc ota staged task_id=%s version=%s path=%s bytes=%llu",
        task_id.c_str(), pending.version.c_str(), kSocOtaImgPath,
        static_cast<unsigned long long>(pending.img_bytes));
    return true;
}

/**
 * 处理 UI 的处置指令：立即重启升级，或放弃并删除固件。
 *
 * RebootNow 分两步执行 updateEngine：先只写 misc 分区拿退出码，坏包能在这里
 * 就被 RKFW 头校验挡下并回 Rsp，不必白重启一次进 recovery 才发现；确认成功
 * 后再自己 sync + reboot，重启时机由本进程掌握。
 */
void MqttService::on_soc_ota_apply(const middleware::Message& msg)
{
    middleware::SocOtaApplyReqPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        UPC_LOG_MQTT_SERVICE(Warn, "invalid UI_Soc_Ota_Apply_Req payload");
        return;
    }
    const auto action = static_cast<middleware::SocOtaApplyAction>(req.action);

    SocOtaPending pending{};
    std::error_code ec;
    if(!soc_ota_pending_load(pending) ||
       !std::filesystem::exists(pending.img_path, ec)) {
        UPC_LOG_MQTT_SERVICE(Warn, "soc ota apply without pending firmware");
        soc_ota_pending_clear();
        publish_soc_ota_apply_rsp(action, middleware::SocOtaApplyResult::NoPending,
                                  "no pending firmware");
        return;
    }

    if(action == middleware::SocOtaApplyAction::Discard) {
        std::filesystem::remove(pending.img_path, ec);
        soc_ota_pending_clear();
        UPC_LOG_MQTT_SERVICE(Info, "soc ota discarded by user task_id=%s",
                             pending.task_id.c_str());
        publish_soc_ota_apply_rsp(action, middleware::SocOtaApplyResult::Ok, "");
        return;
    }

    const std::string misc_cmd = std::string(kSocOtaUpdateEngine) +
        " --image_url=" + sh_single_quote(pending.img_path) + " --misc=update";
    UPC_LOG_MQTT_SERVICE(Info, "soc ota apply: %s", misc_cmd.c_str());
    if(!run_shell_ok(misc_cmd)) {
        UPC_LOG_MQTT_SERVICE(Error, "soc ota updateEngine failed, misc not written");
        publish_soc_ota_apply_rsp(action, middleware::SocOtaApplyResult::ApplyFailed,
                                  "updateEngine failed");
        return;
    }

    /* misc 已写入，重启后 recovery 会接管刷写。pending 保留：新系统起来后
     * 靠比对版本决定是清理（升级成功）还是重新提示（没刷上）。 */
    UPC_LOG_MQTT_SERVICE(Info, "soc ota misc written, rebooting into recovery");
    publish_soc_ota_apply_rsp(action, middleware::SocOtaApplyResult::Ok, "");
    ::sync();
    (void)run_shell_ok("reboot");
}

/**
 * MQTT 首次连上云端后处理遗留的待应用固件。
 *
 * 放在连云成功之后而不是 init()：rongsu.sh 的启动顺序里 MQTT 比 UI 先起，
 * 早发这条 Ntf 时 UI 还没订阅，消息会直接丢掉。
 *
 * 版本相同说明上次重启后 recovery 已经刷成功，本地残留的 .img 和状态文件
 * 该清掉；不同则说明固件还没应用，重新提示用户。云端有自己的心跳版本比对，
 * 这里的比对只用于决定本地文件的去留。
 */
void MqttService::notify_soc_ota_ready_after_connect()
{
    if(soc_ota_ready_notified_) {
        return;
    }
    soc_ota_ready_notified_ = true;

    SocOtaPending pending{};
    if(!soc_ota_pending_load(pending)) {
        return;
    }

    std::error_code ec;
    if(!std::filesystem::exists(pending.img_path, ec)) {
        UPC_LOG_MQTT_SERVICE(Warn, "soc ota pending firmware missing, drop state");
        soc_ota_pending_clear();
        return;
    }

    if(same_firmware_version(pending.version, UPC_SW_VERSION)) {
        UPC_LOG_MQTT_SERVICE(Info,
            "soc ota already applied (version=%s), cleaning up", UPC_SW_VERSION);
        std::filesystem::remove(pending.img_path, ec);
        soc_ota_pending_clear();
        return;
    }

    UPC_LOG_MQTT_SERVICE(Info,
        "soc ota pending firmware version=%s (running %s), re-notify UI",
        pending.version.c_str(), UPC_SW_VERSION);
    publish_soc_ota_ready(pending);
}

void MqttService::publish_soc_ota_ready(const SocOtaPending& pending)
{
    middleware::SocOtaReadyPayload p{};
    std::snprintf(p.version, sizeof(p.version), "%s", pending.version.c_str());
    std::snprintf(p.task_id, sizeof(p.task_id), "%s", pending.task_id.c_str());
    p.img_bytes = pending.img_bytes;

    middleware::Message msg(middleware::MsgType::APP_Soc_Ota_Ready_Ntf,
                            middleware::ModuleId::APP_MQTT,
                            middleware::ModuleId::UI);
    msg.priority = middleware::MsgPriority::High;
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)middleware::MsgPort::instance().publish_async(msg);
}

void MqttService::publish_soc_ota_apply_rsp(middleware::SocOtaApplyAction action,
                                             middleware::SocOtaApplyResult result,
                                             const char* reason)
{
    middleware::SocOtaApplyRspPayload p{};
    p.action = static_cast<uint8_t>(action);
    p.result = static_cast<uint8_t>(result);
    std::snprintf(p.reason, sizeof(p.reason), "%s", reason ? reason : "");

    middleware::Message msg(middleware::MsgType::APP_Soc_Ota_Apply_Rsp,
                            middleware::ModuleId::APP_MQTT,
                            middleware::ModuleId::UI);
    msg.priority = middleware::MsgPriority::High;
    msg.payload = middleware::MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&p), sizeof(p));
    (void)middleware::MsgPort::instance().publish_async(msg);
}

/* ---- 待应用状态的持久化：沿用 MqttCredentialStore 的 key=value + 原子替换 ---- */

bool MqttService::soc_ota_pending_load(SocOtaPending& out) const
{
    out = SocOtaPending{};
    std::ifstream input(kSocOtaPendingFile);
    if(!input.is_open()) {
        return false;
    }
    std::string line;
    while(std::getline(input, line)) {
        const size_t sep = line.find('=');
        if(sep == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, sep);
        const std::string value = line.substr(sep + 1);
        if(key == "taskId") {
            out.task_id = value;
        } else if(key == "version") {
            out.version = value;
        } else if(key == "imgPath") {
            out.img_path = value;
        } else if(key == "imgBytes") {
            out.img_bytes = std::strtoull(value.c_str(), nullptr, 10);
        }
    }
    return !out.img_path.empty();
}

bool MqttService::soc_ota_pending_save(const SocOtaPending& pending) const
{
    if(pending.img_path.empty() ||
       pending.img_path.find_first_of("\r\n") != std::string::npos ||
       pending.version.find_first_of("\r\n") != std::string::npos ||
       pending.task_id.find_first_of("\r\n") != std::string::npos) {
        UPC_LOG_MQTT_SERVICE(Warn, "refuse to save invalid soc ota pending state");
        return false;
    }

    const std::filesystem::path path(kSocOtaPendingFile);
    std::error_code ec;
    if(path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if(ec) {
            return false;
        }
    }

    const std::string temporary = std::string(kSocOtaPendingFile) + ".tmp";
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                          S_IRUSR | S_IWUSR);
    if(fd < 0) {
        return false;
    }
    char buf[512];
    const int len = std::snprintf(buf, sizeof(buf),
        "taskId=%s\nversion=%s\nimgPath=%s\nimgBytes=%llu\n",
        pending.task_id.c_str(), pending.version.c_str(), pending.img_path.c_str(),
        static_cast<unsigned long long>(pending.img_bytes));
    if(len <= 0 || static_cast<size_t>(len) >= sizeof(buf)) {
        (void)::close(fd);
        (void)::unlink(temporary.c_str());
        return false;
    }
    const ssize_t written = ::write(fd, buf, static_cast<size_t>(len));
    const bool write_ok = written == static_cast<ssize_t>(len) &&
                          ::fsync(fd) == 0 && ::close(fd) == 0;
    if(!write_ok) {
        (void)::close(fd);
        (void)::unlink(temporary.c_str());
        return false;
    }
    if(::rename(temporary.c_str(), kSocOtaPendingFile) != 0) {
        (void)::unlink(temporary.c_str());
        return false;
    }
    return true;
}

void MqttService::soc_ota_pending_clear() const
{
    if(::unlink(kSocOtaPendingFile) != 0 && errno != ENOENT) {
        UPC_LOG_MQTT_SERVICE(Warn, "cannot clear soc ota pending state errno=%d", errno);
    }
}

} // namespace upc::mqtt
