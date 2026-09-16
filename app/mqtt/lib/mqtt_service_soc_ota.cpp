#include "mqtt_service.hpp"

#include "mqtt_service_internal.hpp"
#include "rs_log.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

/**
 * 点分版本比较：a<b 返回负，相等 0，a>b 返回正。
 *
 * 段数不等时缺失段按 0 补齐，忽略前导 'V'。不能用字符串比较，否则
 * "V4.10.0" 会被判成小于 "V4.9.0"。
 */
int compare_dotted_version(const std::string& a, const std::string& b)
{
    auto split = [](const std::string& in) {
        std::vector<unsigned long> out;
        std::string s = to_upper_ascii(in);
        if(!s.empty() && s.front() == 'V') {
            s.erase(0, 1);
        }
        size_t pos = 0;
        while(true) {
            const size_t dot = s.find('.', pos);
            const std::string seg = (dot == std::string::npos)
                ? s.substr(pos) : s.substr(pos, dot - pos);
            out.push_back(std::strtoul(seg.c_str(), nullptr, 10));
            if(dot == std::string::npos) {
                break;
            }
            pos = dot + 1;
        }
        return out;
    };
    const std::vector<unsigned long> va = split(a);
    const std::vector<unsigned long> vb = split(b);
    const size_t n = std::max(va.size(), vb.size());
    for(size_t i = 0; i < n; ++i) {
        const unsigned long x = (i < va.size()) ? va[i] : 0ul;
        const unsigned long y = (i < vb.size()) ? vb[i] : 0ul;
        if(x != y) {
            return (x < y) ? -1 : 1;
        }
    }
    return 0;
}

/** 应用层可执行文件名：统一 rongsu_ 前缀，与 process_config_* 的 name 对齐。 */
bool is_app_binary_name(const std::string& name)
{
    return name.rfind("rongsu_", 0) == 0;
}

/**
 * 描述文件没给 files 清单时的兜底必需项。
 *
 * 只拦"本版本已知的进程缺失"，包里多出来的新进程一律放行——做校验的是升级前
 * 的旧版本，它不可能认识新版本新增的进程，卡死这里会让新增进程永远发不出去。
 */
constexpr const char* kAppRequiredBinaries[] = {
    "rongsu_broker", "rongsu_glavo", "rongsu_camera", "rongsu_media",
    "rongsu_xfr",    "rongsu_visual", "rongsu_mqtt",  "rongsu_ui",
};

/** 递归统计目录下常规文件的字节总数，用于 UI 展示占用。 */
uint64_t dir_total_bytes(const std::filesystem::path& dir)
{
    std::error_code ec;
    uint64_t total = 0;
    for(const auto& ent : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if(ec) {
            break;
        }
        if(ent.is_regular_file(ec)) {
            total += static_cast<uint64_t>(ent.file_size(ec));
        }
    }
    return total;
}

} // namespace

/**
 * 解包 SOC 升级包，判别升级方式后分流落盘，写下待应用状态。
 *
 * 与 mcu/galvo 的差别：不走 AB 对规则、不做整目录替换、也不在这里刷写。
 * 刷写/切槽时机交给用户，见 on_soc_ota_apply。
 *
 * 不先 `tar -tzvf` 再解包：gzip 无法随机访问，列目录等于白解压一整遍，
 * SOC 全包上会多出数分钟。改为粗估空间 → 只解包一次 → 用目录内容判别与校验。
 */
bool MqttService::run_soc_ota_stage(const std::string& task_id,
                                    const OtaComponent& comp,
                                    const std::filesystem::path& tar_gz,
                                    middleware::OtaProgressPayload& progress,
                                    std::string& err_code)
{
    err_code.clear();
    std::error_code ec;

    /* 1) 先清掉上一次遗留的待应用固件：新包已下载完，旧的必然废弃，
     *    而且腾出的空间正好给本次解包用。 */
    soc_ota_pending_clear();
    std::filesystem::remove(kSocOtaImgPath, ec);

    /* 2) 粗估空间：解包期间 tar.gz 与 .img 会短暂共存。不知道解压后精确大小时，
     *    按「压缩包的 2 倍 + 余量」估；manifest 带了 size_bytes 则优先用它。 */
    const uint64_t tar_bytes =
        static_cast<uint64_t>(std::filesystem::file_size(tar_gz, ec));
    const uint64_t package_hint =
        (comp.size_bytes > 0) ? comp.size_bytes : (ec ? 0ull : tar_bytes);
    uint64_t free_bytes = 0;
    if(package_hint > 0 && fs_free_bytes(UPC_IMAGE_ROOT, free_bytes)) {
        const uint64_t need = package_hint * 2ull + kSocOtaSpareBytes;
        if(free_bytes < need) {
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error,
                "soc ota no space: need~=%llu free=%llu",
                static_cast<unsigned long long>(need),
                static_cast<unsigned long long>(free_bytes));
            err_code = "no_space";
            return false;
        }
    } else if(package_hint == 0) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "soc ota skip space check (unknown package size)");
    } else {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "soc ota statvfs failed, skip space check");
    }

    progress.state = static_cast<uint8_t>(middleware::OtaProgressState::Applying);
    progress.progress_pct = 95;
    publish_ota_progress_ui(progress);
    (void)publish_ota_progress_cloud(progress);

    /* 3) 解包到 tmp 子目录而不是直接摊到 /userdata：与 kSocOtaImgPath 同一
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

    /* 4) 判别升级方式：优先信包内描述文件，缺失时按解包内容推断，
     *    以兼容加入描述文件之前打好的纯 .img 包。 */
    SocOtaManifest manifest{};
    if(!parse_soc_ota_manifest(extract_dir, manifest)) {
        std::filesystem::remove_all(extract_dir, ec);
        err_code = "manifest_invalid";
        return false;
    }

    /* 包内 version 与云端 manifest 对不上说明打包或分发错配，
     * 继续下去会让云端的心跳版本比对永远判不出升级完成。 */
    if(!manifest.version.empty() && !comp.version.empty() &&
       !same_firmware_version(manifest.version, comp.version)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc ota version mismatch: package=%s cloud=%s",
                             manifest.version.c_str(), comp.version.c_str());
        std::filesystem::remove_all(extract_dir, ec);
        err_code = "version_mismatch";
        return false;
    }

    if(!manifest.min_compat.empty() &&
       compare_dotted_version(UPC_SW_VERSION, manifest.min_compat) < 0) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc ota rejects jump: running=%s min_compat=%s",
                             UPC_SW_VERSION, manifest.min_compat.c_str());
        std::filesystem::remove_all(extract_dir, ec);
        err_code = "incompatible_version";
        return false;
    }

    auto kind = middleware::SocOtaKind::Image;
    if(manifest.present) {
        if(manifest.type == "app") {
            kind = middleware::SocOtaKind::App;
        } else if(manifest.type != "image") {
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc ota unknown manifest type=%s",
                                 manifest.type.c_str());
            std::filesystem::remove_all(extract_dir, ec);
            err_code = "manifest_type_invalid";
            return false;
        }
    } else {
        bool has_img = false;
        bool has_app_bin = false;
        for(const auto& ent : std::filesystem::directory_iterator(extract_dir, ec)) {
            if(ec) {
                break;
            }
            if(!ent.is_regular_file()) {
                continue;
            }
            const std::string name = ent.path().filename().string();
            if(is_img_name(name)) {
                has_img = true;
            } else if(is_app_binary_name(name)) {
                has_app_bin = true;
            }
        }
        if(has_img == has_app_bin) {
            /* 两种都有或两种都没有：无从判断该刷整机还是只换应用，拒绝比猜安全 */
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc ota cannot tell kind (img=%d app=%d)",
                                 static_cast<int>(has_img), static_cast<int>(has_app_bin));
            std::filesystem::remove_all(extract_dir, ec);
            err_code = "kind_ambiguous";
            return false;
        }
        kind = has_img ? middleware::SocOtaKind::Image : middleware::SocOtaKind::App;
    }

    /* 5) 按方式落盘。extract_dir 在 /userdata：全包 rename 到同分区的固定路径
     *    是零拷贝；应用层只有十几 MB，跨分区拷进槽的开销可以忽略。 */
    SocOtaPending pending{};
    pending.task_id = task_id;
    pending.version = comp.version.empty() ? manifest.version : comp.version;
    pending.kind = kind;

    const bool staged = (kind == middleware::SocOtaKind::Image)
        ? stage_soc_image(task_id, comp, extract_dir, pending, err_code)
        : stage_soc_app(task_id, comp, extract_dir, manifest, pending, err_code);
    std::filesystem::remove_all(extract_dir, ec);
    if(!staged) {
        return false;
    }

    /* 6) 记下待应用状态：用户点"稍后"之后可能断电，重启还要能找回入口。 */
    if(!soc_ota_pending_save(pending)) {
        if(kind == middleware::SocOtaKind::Image) {
            std::filesystem::remove(pending.payload_path, ec);
        } else {
            std::filesystem::remove_all(pending.payload_path, ec);
        }
        err_code = "pending_save_failed";
        return false;
    }

    UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info,
        "soc ota staged task_id=%s kind=%s version=%s path=%s bytes=%llu",
        task_id.c_str(),
        (kind == middleware::SocOtaKind::Image) ? "image" : "app",
        pending.version.c_str(), pending.payload_path.c_str(),
        static_cast<unsigned long long>(pending.payload_bytes));
    return true;
}

/**
 * 全包分支：包内必须恰好一个 .img，改名到固定路径。
 *
 * 固定单文件是为了让多次升级不累积几百 MB 的垃圾；描述文件允许与 .img 共存，
 * 校验时跳过它。
 */
bool MqttService::stage_soc_image(const std::string& task_id,
                                  const OtaComponent& comp,
                                  const std::filesystem::path& extract_dir,
                                  SocOtaPending& pending,
                                  std::string& err_code)
{
    (void)task_id;
    (void)comp;
    std::error_code ec;

    std::filesystem::path img_src;
    size_t img_found = 0;
    size_t other_found = 0;
    for(const auto& ent : std::filesystem::directory_iterator(extract_dir, ec)) {
        if(ec) {
            break;
        }
        if(ent.is_directory()) {
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "soc ota reject nested dir=%s",
                                 ent.path().filename().c_str());
            err_code = "tar_entry_invalid";
            return false;
        }
        if(!ent.is_regular_file()) {
            continue;
        }
        const std::string name = ent.path().filename().string();
        if(name == kSocOtaManifestName) {
            continue;
        }
        if(is_img_name(name)) {
            img_src = ent.path();
            ++img_found;
        } else {
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "soc ota reject non-img entry=%s", name.c_str());
            ++other_found;
        }
    }
    if(other_found > 0) {
        err_code = "tar_entry_invalid";
        return false;
    }
    if(img_found != 1) {
        err_code = (img_found == 0) ? "img_missing" : "img_ambiguous";
        return false;
    }

    std::filesystem::rename(img_src, kSocOtaImgPath, ec);
    if(ec) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc ota rename to %s failed: %s",
                             kSocOtaImgPath, ec.message().c_str());
        err_code = "img_install_failed";
        return false;
    }

    const uint64_t final_bytes =
        static_cast<uint64_t>(std::filesystem::file_size(kSocOtaImgPath, ec));
    pending.payload_path = kSocOtaImgPath;
    pending.payload_bytes = ec ? 0ull : final_bytes;
    return true;
}

/**
 * 应用层分支：校验文件清单后整套落到非活动槽，不切槽。
 *
 * 切槽推迟到 on_soc_ota_apply，因为用户可能选"稍后"——提前切了的话，中途
 * 任意一次意外重启都会跑上没被用户确认过的新版本。
 */
bool MqttService::stage_soc_app(const std::string& task_id,
                                const OtaComponent& comp,
                                const std::filesystem::path& extract_dir,
                                const SocOtaManifest& manifest,
                                SocOtaPending& pending,
                                std::string& err_code)
{
    (void)task_id;
    (void)comp;
    std::error_code ec;

    /* 1) 目标槽取当前运行槽的另一个。判不出当前槽说明设备还是迁移前的扁平
     *    布局，此时写槽只会造出一个谁也不会启动的目录，不如明确失败。 */
    std::string cur_slot;
    if(!app_slot_current(cur_slot)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc app ota needs A/B layout, exe not in a slot");
        err_code = "slot_layout_missing";
        return false;
    }
    const std::string target_slot = (cur_slot == kAppSlotA) ? kAppSlotB : kAppSlotA;
    const std::filesystem::path slot_dir = std::filesystem::path(kAppSlotRoot) / target_slot;
    const std::filesystem::path staging_dir =
        std::filesystem::path(kAppSlotRoot) / (target_slot + ".new");

    /* 2) 清单校验：描述文件给了就以它为准（未来删进程也能通过），
     *    没给才用本版本已知的必需项兜底。槽内启动脚本无论如何都得有，
     *    缺了新槽起不来，bootstrap 会退回扁平布局找不到东西。 */
    std::vector<std::string> present;
    uint64_t payload_bytes = 0;
    for(const auto& ent : std::filesystem::directory_iterator(extract_dir, ec)) {
        if(ec) {
            break;
        }
        if(ent.is_directory()) {
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "soc app ota reject nested dir=%s",
                                 ent.path().filename().c_str());
            err_code = "tar_entry_invalid";
            return false;
        }
        if(!ent.is_regular_file()) {
            continue;
        }
        const std::string name = ent.path().filename().string();
        if(name == kSocOtaManifestName) {
            continue;
        }
        present.push_back(name);
        payload_bytes += static_cast<uint64_t>(ent.file_size(ec));
    }

    auto has_entry = [&present](const std::string& name) {
        return std::find(present.begin(), present.end(), name) != present.end();
    };
    if(!manifest.files.empty()) {
        for(const std::string& want : manifest.files) {
            if(want != kSocOtaManifestName && !has_entry(want)) {
                UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc app ota missing declared file=%s",
                                     want.c_str());
                err_code = "app_file_missing";
                return false;
            }
        }
    } else {
        for(const char* want : kAppRequiredBinaries) {
            if(!has_entry(want)) {
                UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc app ota missing binary=%s", want);
                err_code = "app_file_missing";
                return false;
            }
        }
    }
    if(!has_entry(kAppSlotScriptName)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc app ota missing %s", kAppSlotScriptName);
        err_code = "app_file_missing";
        return false;
    }

    /* 3) 先删非活动槽再落盘：当前跑的是 cur_slot，回滚目标也是它，target_slot
     *    里存的是更早的版本，删掉不损失任何回滚能力，却能把槽根分区的峰值占用
     *    从三份压到两份。 */
    std::filesystem::remove_all(slot_dir, ec);
    std::filesystem::remove_all(staging_dir, ec);

    uint64_t free_bytes = 0;
    if(fs_free_bytes(kAppSlotRoot, free_bytes) &&
       free_bytes < payload_bytes + kAppSlotSpareBytes) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc app ota no space: need=%llu free=%llu",
            static_cast<unsigned long long>(payload_bytes + kAppSlotSpareBytes),
            static_cast<unsigned long long>(free_bytes));
        err_code = "no_space";
        return false;
    }

    /* 4) 先落到 .new 再整目录改名：拷贝中途掉电只会留下一个残缺的 .new，
     *    下次升级开头就会清掉，不会让槽本身变成半新半旧。 */
    if(!ensure_dir(staging_dir)) {
        err_code = "slot_dir_failed";
        return false;
    }
    for(const std::string& name : present) {
        std::filesystem::copy_file(extract_dir / name, staging_dir / name,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if(ec) {
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc app ota copy %s failed: %s",
                                 name.c_str(), ec.message().c_str());
            std::filesystem::remove_all(staging_dir, ec);
            err_code = "app_install_failed";
            return false;
        }
        /* 包里的权限位经 tar 往返未必可靠，统一补上可执行位 */
        std::filesystem::permissions(staging_dir / name,
            std::filesystem::perms::owner_all |
            std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace, ec);
    }

    /* 改名前落盘：rename 只保证目录项原子，不保证文件内容已经写下去 */
    ::sync();
    std::filesystem::rename(staging_dir, slot_dir, ec);
    if(ec) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc app ota rename to %s failed: %s",
                             slot_dir.c_str(), ec.message().c_str());
        std::filesystem::remove_all(staging_dir, ec);
        err_code = "app_install_failed";
        return false;
    }

    pending.payload_path = slot_dir.string();
    pending.payload_bytes = dir_total_bytes(slot_dir);
    pending.target_slot = target_slot;
    UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info, "soc app ota staged into %s (running %s)",
                         target_slot.c_str(), cur_slot.c_str());
    return true;
}

/**
 * 读取包内 ota_manifest.json。
 *
 * 文件不存在返回 true 且 present=false —— 那是合法的老格式包，不是错误。
 */
bool MqttService::parse_soc_ota_manifest(const std::filesystem::path& extract_dir,
                                         SocOtaManifest& out)
{
    out = SocOtaManifest{};
    const std::filesystem::path path = extract_dir / kSocOtaManifestName;

    std::error_code ec;
    if(!std::filesystem::exists(path, ec)) {
        return true;
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if(ec || size == 0 || size > 64u * 1024u) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc ota manifest size invalid");
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if(!input.is_open()) {
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());

    out.present = true;
    if(!json_extract_string(text, "type", out.type)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc ota manifest missing type");
        return false;
    }
    (void)json_extract_string(text, "version", out.version);
    (void)json_extract_string(text, "min_compat", out.min_compat);
    (void)json_extract_string_array(text, "files", out.files);
    return true;
}

/** 从 /proc/self/exe 反推当前槽，见头文件对取值来源的说明。 */
bool MqttService::app_slot_current(std::string& out)
{
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if(n <= 0) {
        return false;
    }
    buf[n] = '\0';

    const std::filesystem::path exe(buf);
    const std::filesystem::path slot_path = exe.parent_path();
    const std::string slot = slot_path.filename().string();
    if(slot != kAppSlotA && slot != kAppSlotB) {
        return false;
    }
    /* 同时比对槽根，避免别处恰好也有个叫 APP1 的目录被认成槽 */
    if(slot_path.parent_path() != std::filesystem::path(kAppSlotRoot)) {
        return false;
    }
    out = slot;
    return true;
}

/** 槽状态文件由 bootstrap 原子重写，这里只读，不需要加锁。 */
bool MqttService::app_slot_state_get(const char* key, std::string& out)
{
    std::ifstream input(kAppSlotStateFile);
    if(!input.is_open()) {
        return false;
    }
    const std::string prefix = std::string(key) + "=";
    std::string line;
    while(std::getline(input, line)) {
        if(line.rfind(prefix, 0) == 0) {
            out = trim_line(line.substr(prefix.size()));
            return true;
        }
    }
    return false;
}

/**
 * 处理 UI 的处置指令：立即重启升级，或放弃并删除固件。
 *
 * 全包的 RebootNow 分两步执行 updateEngine：先只写 misc 分区拿退出码，坏包能
 * 在这里就被 RKFW 头校验挡下并回 Rsp，不必白重启一次进 recovery 才发现；确认
 * 成功后再自己 sync + reboot，重启时机由本进程掌握。
 *
 * 应用层的 RebootNow 只是切槽再重启，真正的生效判定交给启动脚本的试运行机制。
 */
void MqttService::on_soc_ota_apply(const middleware::Message& msg)
{
    middleware::SocOtaApplyReqPayload req{};
    if(!read_inline_pod(msg.payload, req)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "invalid UI_Soc_Ota_Apply_Req payload");
        return;
    }
    const auto action = static_cast<middleware::SocOtaApplyAction>(req.action);

    SocOtaPending pending{};
    std::error_code ec;
    if(!soc_ota_pending_load(pending) ||
       !std::filesystem::exists(pending.payload_path, ec)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "soc ota apply without pending firmware");
        soc_ota_pending_clear();
        publish_soc_ota_apply_rsp(action, middleware::SocOtaApplyResult::NoPending,
                                  "no pending firmware");
        return;
    }
    const bool is_app = pending.kind == middleware::SocOtaKind::App;

    if(action == middleware::SocOtaApplyAction::Discard) {
        /* 应用层的 payload 是整个非活动槽目录，全包是单个 .img */
        if(is_app) {
            std::filesystem::remove_all(pending.payload_path, ec);
        } else {
            std::filesystem::remove(pending.payload_path, ec);
        }
        soc_ota_pending_clear();
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info, "soc ota discarded by user task_id=%s",
                             pending.task_id.c_str());
        publish_soc_ota_apply_rsp(action, middleware::SocOtaApplyResult::Ok, "");
        return;
    }

    if(is_app) {
        /* 切槽交给 bootstrap：状态文件的字段语义与原子写只在脚本里实现一份，
         * 免得 C++ 和 shell 两边各写一套解析、日后改出分歧。 */
        const std::string switch_cmd = std::string(kAppBootstrap) + " slot-switch " +
            sh_single_quote(pending.target_slot) + " " + sh_single_quote(pending.version);
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info, "soc app ota apply: %s", switch_cmd.c_str());
        if(!run_shell_ok(switch_cmd)) {
            UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc app ota slot switch failed, slot unchanged");
            publish_soc_ota_apply_rsp(action,
                                      middleware::SocOtaApplyResult::SlotSwitchFailed,
                                      "slot switch failed");
            return;
        }
        /* pending 保留：新槽起来后靠比对版本决定是清理还是重新提示，回滚时
         * 则由启动脚本置的 rollback 标记来丢弃。 */
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info, "soc app ota switched to %s, rebooting",
                             pending.target_slot.c_str());
        publish_soc_ota_apply_rsp(action, middleware::SocOtaApplyResult::Ok, "");
        ::sync();
        (void)run_shell_ok("reboot");
        return;
    }

    const std::string misc_cmd = std::string(kSocOtaUpdateEngine) +
        " --image_url=" + sh_single_quote(pending.payload_path) + " --misc=update";
    UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info, "soc ota apply: %s", misc_cmd.c_str());
    if(!run_shell_ok(misc_cmd)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Error, "soc ota updateEngine failed, misc not written");
        publish_soc_ota_apply_rsp(action, middleware::SocOtaApplyResult::ApplyFailed,
                                  "updateEngine failed");
        return;
    }

    /* misc 已写入，重启后 recovery 会接管刷写。pending 保留：新系统起来后
     * 靠比对版本决定是清理（升级成功）还是重新提示（没刷上）。 */
    UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info, "soc ota misc written, rebooting into recovery");
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

    /* 启动脚本判定新槽跑不稳并回滚后会置 rollback=1。待应用状态必须在这里丢弃：
     * 否则又弹一次"是否重启升级"，用户点了还会被回滚，陷入提示死循环。 */
    std::string rollback;
    if(app_slot_state_get("rollback", rollback) && rollback == "1") {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn,
            "app slot rolled back, dropping pending soc ota task_id=%s version=%s",
            pending.task_id.c_str(), pending.version.c_str());
        if(pending.kind == middleware::SocOtaKind::App) {
            std::filesystem::remove_all(pending.payload_path, ec);
        }
        soc_ota_pending_clear();
        return;
    }

    if(!std::filesystem::exists(pending.payload_path, ec)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "soc ota pending firmware missing, drop state");
        soc_ota_pending_clear();
        return;
    }

    if(same_firmware_version(pending.version, UPC_SW_VERSION)) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info,
            "soc ota already applied (version=%s), cleaning up", UPC_SW_VERSION);
        /* 应用层的 payload 就是当前正在运行的槽，只清状态；全包才删那个
         * 几百 MB 的 .img。 */
        if(pending.kind == middleware::SocOtaKind::Image) {
            std::filesystem::remove(pending.payload_path, ec);
        }
        soc_ota_pending_clear();
        return;
    }

    UPC_LOG_MQTT_SERVICE_TO(MqttOta, Info,
        "soc ota pending firmware version=%s (running %s), re-notify UI",
        pending.version.c_str(), UPC_SW_VERSION);
    publish_soc_ota_ready(pending);
}

void MqttService::publish_soc_ota_ready(const SocOtaPending& pending)
{
    middleware::SocOtaReadyPayload p{};
    std::snprintf(p.version, sizeof(p.version), "%s", pending.version.c_str());
    std::snprintf(p.task_id, sizeof(p.task_id), "%s", pending.task_id.c_str());
    p.img_bytes = pending.payload_bytes;
    p.kind = static_cast<uint8_t>(pending.kind);

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
        } else if(key == "payloadPath" || key == "imgPath") {
            /* imgPath/imgBytes 是加入应用层升级之前的键名。留着兼容读取，
             * 否则跨这次升级时旧状态读不出来，那个几百 MB 的 .img 就没人清了。 */
            out.payload_path = value;
        } else if(key == "payloadBytes" || key == "imgBytes") {
            out.payload_bytes = std::strtoull(value.c_str(), nullptr, 10);
        } else if(key == "kind") {
            out.kind = (value == "app") ? middleware::SocOtaKind::App
                                        : middleware::SocOtaKind::Image;
        } else if(key == "targetSlot") {
            out.target_slot = value;
        }
    }
    /* 应用层没记下目标槽就无从切槽，视为状态损坏 */
    if(out.kind == middleware::SocOtaKind::App && out.target_slot.empty()) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "soc ota pending state lacks target slot");
        return false;
    }
    return !out.payload_path.empty();
}

bool MqttService::soc_ota_pending_save(const SocOtaPending& pending) const
{
    if(pending.payload_path.empty() ||
       pending.payload_path.find_first_of("\r\n") != std::string::npos ||
       pending.version.find_first_of("\r\n") != std::string::npos ||
       pending.task_id.find_first_of("\r\n") != std::string::npos ||
       pending.target_slot.find_first_of("\r\n") != std::string::npos) {
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "refuse to save invalid soc ota pending state");
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
        "taskId=%s\nversion=%s\nkind=%s\npayloadPath=%s\npayloadBytes=%llu\n"
        "targetSlot=%s\n",
        pending.task_id.c_str(), pending.version.c_str(),
        (pending.kind == middleware::SocOtaKind::App) ? "app" : "image",
        pending.payload_path.c_str(),
        static_cast<unsigned long long>(pending.payload_bytes),
        pending.target_slot.c_str());
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
        UPC_LOG_MQTT_SERVICE_TO(MqttOta, Warn, "cannot clear soc ota pending state errno=%d", errno);
    }
}

} // namespace upc::mqtt
