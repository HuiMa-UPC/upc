/**
 * @file wifi_manager.cpp
 * @brief WifiManager 实现：直接驱动 wpa_supplicant 控制接口（wpa_ctrl / libwpa_client）。
 *
 * 目标板由自定义脚本启动 wpa_supplicant（wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf，
 * 其中 ctrl_interface=/var/run/wpa_supplicant、update_config=1），故本类直接连
 * /var/run/wpa_supplicant/<iface> 控制 socket 收发命令，DHCP 用 udhcpc。
 *
 * 线程模型：一个后台线程，命令队列 + 周期轮询（默认 3s）。命令由公开接口投递，
 * 轮询里读 STATUS/SIGNAL_POLL/SCAN_RESULTS 并按需回调（state 变化才推、扫描列表变化才推）。
 * 回调在该线程触发，内部只做 publish（MsgPort::publish_async 线程安全）。
 */

 #include "wifi_manager.hpp"

 #include "rs_log.hpp"
 
 #include <wpa_ctrl.h>
 
 #include <arpa/inet.h>
 #include <ifaddrs.h>
 #include <net/if.h>
 #include <sys/types.h>
 
 #include <atomic>
 #include <chrono>
 #include <condition_variable>
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>
 #include <deque>
 #include <mutex>
 #include <string>
 #include <thread>
 #include <vector>
 
 namespace upc::glavo {
 
 namespace {
 
 constexpr int kPollIntervalMs = 3000;
 constexpr int kDhcpTimeoutSec = 8;
 
 const char* wifi_iface()
 {
     const char* e = std::getenv("UPC_WIFI_IFACE");
     return (e && e[0]) ? e : "wlan0";
 }
 
 std::string wpa_ctrl_socket_path()
 {
     const char* e = std::getenv("UPC_WPA_CTRL_DIR");
     std::string dir = (e && e[0]) ? e : "/var/run/wpa_supplicant";
     return dir + "/" + wifi_iface();
 }
 
 /** RSSI(dBm) → 信号格数(0..4)。 */
 int dbm_to_bars(int dbm)
 {
     if(dbm >= -50) return 4;
     if(dbm >= -60) return 3;
     if(dbm >= -70) return 2;
     if(dbm >= -80) return 1;
     return 0;
 }
 
 /** 从 "key=value\n..." 文本块取某个 key 的值。 */
 std::string kv_get(const std::string& block, const char* key)
 {
     const std::string k = std::string(key) + "=";
     size_t pos = 0;
     while(pos < block.size()) {
         size_t eol = block.find('\n', pos);
         if(eol == std::string::npos) eol = block.size();
         if(block.compare(pos, k.size(), k) == 0) {
             return block.substr(pos + k.size(), eol - (pos + k.size()));
         }
         pos = eol + 1;
     }
     return {};
 }
 
 /** flags 文本 → 契约安全类型。 */
 WifiSecurity flags_to_security(const std::string& flags)
 {
     if(flags.find("WPA3") != std::string::npos ||
        flags.find("SAE")  != std::string::npos) {
         return WifiSecurity::Wpa3;
     }
     if(flags.find("WPA")  != std::string::npos ||
        flags.find("RSN")  != std::string::npos ||
        flags.find("PSK")  != std::string::npos ||
        flags.find("WEP")  != std::string::npos) {
         return WifiSecurity::Wpa2;
     }
     return WifiSecurity::Open;
 }
 
 /** 读取指定网卡的 IPv4 文本，无则空串。 */
 std::string iface_ipv4()
 {
     std::string ip;
     struct ifaddrs* ifa = nullptr;
     if(getifaddrs(&ifa) != 0) {
         return ip;
     }
     const char* want = wifi_iface();
     for(struct ifaddrs* p = ifa; p; p = p->ifa_next) {
         if(!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
         if(!p->ifa_name || std::strcmp(p->ifa_name, want) != 0) continue;
         char buf[INET_ADDRSTRLEN] = {0};
         auto* sin = reinterpret_cast<struct sockaddr_in*>(p->ifa_addr);
         if(inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
             ip = buf;
         }
         break;
     }
     freeifaddrs(ifa);
     return ip;
 }
 
 /** 按分隔符切分一行。 */
 std::vector<std::string> split(const std::string& s, char sep)
 {
     std::vector<std::string> out;
     size_t start = 0;
     while(true) {
         size_t pos = s.find(sep, start);
         if(pos == std::string::npos) {
             out.push_back(s.substr(start));
             break;
         }
         out.push_back(s.substr(start, pos - start));
         start = pos + 1;
     }
     return out;
 }
 
 } // namespace
 
 /* ============================================================================
  * Impl
  * ============================================================================ */
 
 struct WifiManager::Impl {
     enum class CmdType { Enable, Scan, Connect, Disconnect, Forget, Info, Saved };
     struct Command {
         CmdType     type;
         bool        flag = false;   /**< Enable 用 */
         std::string ssid;
         std::string pw;
     };
 
     struct wpa_ctrl* ctrl = nullptr;
     bool             ctrl_was_open = false;
 
     std::thread            thread;
     std::atomic<bool>      running{false};
     std::mutex             mtx;
     std::condition_variable cv;
     std::deque<Command>    cmds;
 
     /* 仅 worker 线程访问 */
     bool        enabled = true;
     bool        want_scan_push = false;
     bool        want_saved_push = false;
     bool        force_state_push = false;
     std::string last_scan_sig;
     std::string dhcp_done_ssid;
 
     /* 连接结果监视：do_connect 后开始，poll 里判定成功/超时 */
     bool        connecting = false;
     std::string connecting_ssid;
     int         connecting_id = -1;        /**< 本次连接使用的 network id */
     bool        connecting_was_new = false;/**< 是否本次新加（失败时需删除，不留在已保存） */
     std::chrono::steady_clock::time_point connect_deadline;
 
     mutable std::mutex snap_mutex;
     WifiSnapshot       snap;
 
     StateCb  state_cb;
     ListCb   list_cb;
     ResultCb result_cb;
 
     void run();
     bool ensure_ctrl();
     bool request(const char* cmd, std::string& out);
     bool request_ok(const char* cmd);
 
     void poll_once();
     void handle_cmd(const Command& c);
     void do_connect(const std::string& ssid, const std::string& pw);
     int  find_network_id(const std::string& ssid);
     void maybe_push_state(const WifiSnapshot& s);
     void push_scan(const std::string& cur_ssid);
     void push_saved(const std::string& cur_ssid);
     void run_dhcp();
 
     void enqueue(Command c)
     {
         {
             std::lock_guard<std::mutex> lk(mtx);
             cmds.push_back(std::move(c));
         }
         cv.notify_one();
     }
 };
 
 /* ---------- 线程主循环 ---------- */
 void WifiManager::Impl::run()
 {
     while(running.load()) {
         std::deque<Command> local;
         {
             std::unique_lock<std::mutex> lk(mtx);
             cv.wait_for(lk, std::chrono::milliseconds(kPollIntervalMs),
                         [this]() { return !running.load() || !cmds.empty(); });
             if(!running.load()) break;
             local.swap(cmds);
         }
         for(const auto& c : local) {
             handle_cmd(c);
         }
         poll_once();
     }
     if(ctrl) {
         wpa_ctrl_close(ctrl);
         ctrl = nullptr;
     }
 }
 
 bool WifiManager::Impl::ensure_ctrl()
 {
     if(ctrl) return true;
     ctrl = wpa_ctrl_open(wpa_ctrl_socket_path().c_str());
     if(!ctrl) {
         if(ctrl_was_open) {
             UPC_LOG_GLAVO_SERVICE(Warn, "wifi: wpa_ctrl_open failed (%s)",
                                   wpa_ctrl_socket_path().c_str());
             ctrl_was_open = false;
         }
         return false;
     }
     ctrl_was_open = true;
     UPC_LOG_GLAVO_SERVICE(Info, "wifi: wpa_ctrl connected (%s)",
                           wpa_ctrl_socket_path().c_str());
     return true;
 }
 
 bool WifiManager::Impl::request(const char* cmd, std::string& out)
 {
     if(!ensure_ctrl()) return false;
     char buf[8192];
     size_t len = sizeof(buf) - 1;
     const int r = wpa_ctrl_request(ctrl, cmd, std::strlen(cmd), buf, &len, nullptr);
     if(r < 0) {
         /* 连接断了：关闭以便下次重开 */
         wpa_ctrl_close(ctrl);
         ctrl = nullptr;
         return false;
     }
     buf[len] = '\0';
     out.assign(buf, len);
     return true;
 }
 
 bool WifiManager::Impl::request_ok(const char* cmd)
 {
     std::string out;
     if(!request(cmd, out)) return false;
     return out.compare(0, 2, "OK") == 0;
 }
 
 /* ---------- 命令 ---------- */
 void WifiManager::Impl::handle_cmd(const Command& c)
 {
     switch(c.type) {
     case CmdType::Enable:
         enabled = c.flag;
         if(enabled) {
             request_ok("ENABLE_NETWORK all");
             request_ok("RECONNECT");
             request_ok("SCAN");
             want_scan_push = true;
         } else {
             request_ok("DISCONNECT");
         }
         force_state_push = true;
         break;
     case CmdType::Scan:
         request_ok("SCAN");
         want_scan_push = true;
         break;
     case CmdType::Connect:
         do_connect(c.ssid, c.pw);
         break;
     case CmdType::Disconnect:
         request_ok("DISCONNECT");
         break;
     case CmdType::Forget: {
         const int id = find_network_id(c.ssid);
         if(id >= 0) {
             char cmd[64];
             std::snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", id);
             request_ok(cmd);
             request_ok("SAVE_CONFIG");
         }
         want_saved_push = true;
         break;
     }
     case CmdType::Info:
         force_state_push = true;
         break;
     case CmdType::Saved:
         want_saved_push = true;
         break;
     }
 }
 
 void WifiManager::Impl::do_connect(const std::string& ssid, const std::string& pw)
 {
     int id = find_network_id(ssid);
     const bool existing = (id >= 0);
     if(!existing) {
         std::string r;
         if(!request("ADD_NETWORK", r)) return;
         id = std::atoi(r.c_str());
         if(id < 0) return;
         char cmd[256];
         std::snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid \"%s\"", id, ssid.c_str());
         request_ok(cmd);
     }
 
     char cmd[256];
     if(!pw.empty()) {
         /* 用户输入了密码：设置/更新 psk */
         std::snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"", id, pw.c_str());
         request_ok(cmd);
     } else if(!existing) {
         /* 全新网络且无密码：视为开放网络 */
         std::snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", id);
         request_ok(cmd);
     }
     /* 已保存网络且未输密码：直接 select，保留原有凭据（不动 psk/key_mgmt） */
 
     std::snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
     request_ok(cmd);
     /* 注意：此处不 SAVE_CONFIG。只有连接成功后才持久化到已保存列表；
      * 失败（如密码错）则丢弃刚加的网络，不写入已保存。 */
 
     dhcp_done_ssid.clear();     /* 连接后强制重新 DHCP */
     want_scan_push  = true;
     force_state_push = true;
 
     /* 开始连接结果监视 */
     connecting         = true;
     connecting_ssid    = ssid;
     connecting_id      = id;
     connecting_was_new = !existing;
     connect_deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(20);
 }
 
 int WifiManager::Impl::find_network_id(const std::string& ssid)
 {
     std::string out;
     if(!request("LIST_NETWORKS", out)) return -1;
     /* 首行是表头：network id / ssid / bssid / flags */
     const std::vector<std::string> lines = split(out, '\n');
     for(size_t i = 1; i < lines.size(); i++) {
         if(lines[i].empty()) continue;
         const std::vector<std::string> f = split(lines[i], '\t');
         if(f.size() >= 2 && f[1] == ssid) {
             return std::atoi(f[0].c_str());
         }
     }
     return -1;
 }
 
 /* ---------- 轮询 ---------- */
 void WifiManager::Impl::run_dhcp()
 {
    /* 先清掉接口上残留的旧 IP（切换网络时旧租约不会自动释放，否则会一直显示旧 IP），
     * 再向新网络请求动态地址。 */
     const char* ifc = wifi_iface();
     char cmd[256];
     std::snprintf(cmd, sizeof(cmd),
                  "ip addr flush dev %s 2>/dev/null; "
                  "udhcpc -i %s -n -q -t %d >/dev/null 2>&1",
                  ifc, ifc, kDhcpTimeoutSec);
     (void)std::system(cmd);
 }
 
 void WifiManager::Impl::poll_once()
 {
     WifiSnapshot s;
     s.enabled = enabled;
 
     if(!ensure_ctrl()) {
         s.initialized = false;
         maybe_push_state(s);
         return;
     }
     s.initialized = true;
 
     std::string status;
     std::string wpa_state, ssid;
     if(request("STATUS", status)) {
         wpa_state = kv_get(status, "wpa_state");
         ssid      = kv_get(status, "ssid");
     }
     s.connected = enabled && (wpa_state == "COMPLETED");
     if(s.connected) {
         s.ssid = ssid;
 
         std::string sp;
         if(request("SIGNAL_POLL", sp)) {
             const std::string rssi = kv_get(sp, "RSSI");
             if(!rssi.empty()) {
                 s.signal_bars = dbm_to_bars(std::atoi(rssi.c_str()));
             }
         }
 
        /* 每连上一个"新" SSID 都重新做 DHCP（动态获取该网络网段的地址），
         * 而不是仅在无 IP 时——否则切换 WiFi 后会一直沿用上一个网络的旧 IP。 */
         if(dhcp_done_ssid != s.ssid) {
             run_dhcp();
             dhcp_done_ssid = s.ssid;
             
         }
         s.ip = iface_ipv4();
     } else {
         dhcp_done_ssid.clear();
     }
 
     maybe_push_state(s);
 
     /* 连接结果判定：连上目标 SSID = 成功；超时未连上 = 失败（如密码错） */
     if(connecting) {
         if(s.connected && s.ssid == connecting_ssid) {
             connecting = false;
             /* 成功：重新启用所有网络（避免 SELECT_NETWORK 把其它网络置 disabled
              * 被持久化，影响下次自动重连），再持久化到已保存列表。 */
             request_ok("ENABLE_NETWORK all");
             request_ok("SAVE_CONFIG");
             want_saved_push = true;
             want_scan_push  = true;   /* 刷新可用列表的 saved/connected 标记 */
             if(result_cb) result_cb(true, connecting_ssid);
         } else if(std::chrono::steady_clock::now() >= connect_deadline) {
             connecting = false;
             /* 失败：新加的网络直接删除（不留在已保存）；不做 SAVE_CONFIG，
              * 文件里仍是之前的良好配置。恢复其它网络的启用状态。 */
             if(connecting_was_new && connecting_id >= 0) {
                 char cmd[64];
                 std::snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", connecting_id);
                 request_ok(cmd);
             }
             request_ok("ENABLE_NETWORK all");
             want_saved_push = true;
             want_scan_push  = true;   /* 已删除失败网络，刷新可用列表 saved 标记 */
             if(result_cb) result_cb(false, connecting_ssid);
         }
     }
 
     if(enabled) {
         push_scan(s.ssid);
     }
     if(want_saved_push) {
         push_saved(s.ssid);
         want_saved_push = false;
     }
 }
 
 void WifiManager::Impl::maybe_push_state(const WifiSnapshot& s)
 {
     bool changed;
     {
         std::lock_guard<std::mutex> lk(snap_mutex);
         changed = (snap.initialized != s.initialized) ||
                   (snap.enabled     != s.enabled)     ||
                   (snap.connected   != s.connected)   ||
                   (snap.signal_bars != s.signal_bars) ||
                   (snap.ssid        != s.ssid)        ||
                   (snap.ip          != s.ip);
         snap = s;
     }
     if((changed || force_state_push) && state_cb) {
         state_cb(s);
     }
     force_state_push = false;
 }
 
 void WifiManager::Impl::push_scan(const std::string& cur_ssid)
 {
     std::string res;
     if(!request("SCAN_RESULTS", res)) return;
 
     /* 已保存 SSID 集合（用于标记 saved） */
     std::string ln;
     (void)request("LIST_NETWORKS", ln);
     const std::vector<std::string> saved_lines = split(ln, '\n');
 
     auto is_saved = [&](const std::string& ssid) {
         for(size_t i = 1; i < saved_lines.size(); i++) {
             const std::vector<std::string> f = split(saved_lines[i], '\t');
             if(f.size() >= 2 && f[1] == ssid) return true;
         }
         return false;
     };
 
     /* SCAN_RESULTS：首行表头，随后每行 bssid\tfreq\tsignal\tflags\tssid */
     std::vector<WifiEntry> entries;
     const std::vector<std::string> lines = split(res, '\n');
     for(size_t i = 1; i < lines.size(); i++) {
         if(lines[i].empty()) continue;
         const std::vector<std::string> f = split(lines[i], '\t');
         if(f.size() < 5) continue;
         const std::string& ssid = f[4];
         if(ssid.empty()) continue;   /* 隐藏网络跳过 */
 
         const int bars = dbm_to_bars(std::atoi(f[2].c_str()));
 
         /* 同 SSID 去重：保留信号更强者 */
         bool merged = false;
         for(auto& e : entries) {
             if(e.ssid == ssid) {
                 if(bars > e.signal_bars) e.signal_bars = bars;
                 merged = true;
                 break;
             }
         }
         if(merged) continue;
 
         WifiEntry e;
         e.ssid        = ssid;
         e.signal_bars = bars;
         e.security    = flags_to_security(f[3]);
         e.saved       = is_saved(ssid);
         e.connected   = (!cur_ssid.empty() && ssid == cur_ssid);
         entries.push_back(std::move(e));
     }
 
     /* 仅在结果集变化或被显式请求时才推，避免频繁重建 UI */
     std::string sig;
     for(const auto& e : entries) {
         sig += e.ssid;
         sig += char('0' + e.signal_bars);
         sig += e.connected ? 'C' : '.';
         sig += ';';
     }
     if(sig == last_scan_sig && !want_scan_push) {
         return;
     }
     last_scan_sig  = sig;
     want_scan_push = false;
     if(list_cb) {
         list_cb(WifiListKind::Scan, entries);
     }
 }
 
 void WifiManager::Impl::push_saved(const std::string& cur_ssid)
 {
     std::string out;
     if(!request("LIST_NETWORKS", out)) return;
 
     std::vector<WifiEntry> entries;
     const std::vector<std::string> lines = split(out, '\n');
     for(size_t i = 1; i < lines.size(); i++) {
         if(lines[i].empty()) continue;
         const std::vector<std::string> f = split(lines[i], '\t');
         if(f.size() < 2 || f[1].empty()) continue;
         WifiEntry e;
         e.ssid      = f[1];
         e.saved     = true;
         e.connected = (!cur_ssid.empty() && f[1] == cur_ssid) ||
                       (f.size() >= 4 && f[3].find("CURRENT") != std::string::npos);
         entries.push_back(std::move(e));
     }
     if(list_cb) {
         list_cb(WifiListKind::Saved, entries);
     }
 }
 
 /* ============================================================================
  * WifiManager 门面
  * ============================================================================ */
 
 WifiManager& WifiManager::instance()
 {
     static WifiManager inst;
     return inst;
 }
 
 WifiManager::~WifiManager()
 {
     shutdown();
 }
 
 bool WifiManager::init(StateCb state_cb, ListCb list_cb, ResultCb result_cb)
 {
     if(impl_) {
         impl_->state_cb  = std::move(state_cb);
         impl_->list_cb   = std::move(list_cb);
         impl_->result_cb = std::move(result_cb);
         return true;
     }
     impl_ = new Impl();
     impl_->state_cb  = std::move(state_cb);
     impl_->list_cb   = std::move(list_cb);
     impl_->result_cb = std::move(result_cb);
     impl_->running.store(true);
     impl_->thread = std::thread([this]() { impl_->run(); });
     return true;
 }
 
 void WifiManager::shutdown()
 {
     if(!impl_) return;
     impl_->running.store(false);
     impl_->cv.notify_all();
     if(impl_->thread.joinable()) {
         impl_->thread.join();
     }
     delete impl_;
     impl_ = nullptr;
 }
 
 void WifiManager::set_enabled(bool enable)
 {
     if(!impl_) return;
     impl_->enqueue({ Impl::CmdType::Enable, enable, {}, {} });
 }
 
 void WifiManager::scan()
 {
     if(!impl_) return;
     impl_->enqueue({ Impl::CmdType::Scan, false, {}, {} });
 }
 
 void WifiManager::connect(const std::string& ssid, const std::string& password)
 {
     if(!impl_) return;
     impl_->enqueue({ Impl::CmdType::Connect, false, ssid, password });
 }
 
 void WifiManager::disconnect(const std::string& ssid)
 {
     if(!impl_) return;
     impl_->enqueue({ Impl::CmdType::Disconnect, false, ssid, {} });
 }
 
 void WifiManager::forget(const std::string& ssid)
 {
     if(!impl_) return;
     impl_->enqueue({ Impl::CmdType::Forget, false, ssid, {} });
 }
 
 void WifiManager::request_info()
 {
     if(!impl_) return;
     impl_->enqueue({ Impl::CmdType::Info, false, {}, {} });
 }
 
 void WifiManager::request_saved()
 {
     if(!impl_) return;
     impl_->enqueue({ Impl::CmdType::Saved, false, {}, {} });
 }
 
 WifiSnapshot WifiManager::snapshot() const
 {
     if(!impl_) return WifiSnapshot{};
     std::lock_guard<std::mutex> lk(impl_->snap_mutex);
     return impl_->snap;
 }
 
 } // namespace upc::glavo
 