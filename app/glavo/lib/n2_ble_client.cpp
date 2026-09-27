/**
 * @file n2_ble_client.cpp
 * @brief 制氮机 BLE：扫描 N2_Machine → Connect（带 ADDR_TYPE）→ Notify 0x1B9A → 解析 ASCII。
 */

#include "n2_ble_client.hpp"
#include "n2_ble_transport.hpp"
#include "rs_log.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace upc::glavo {
namespace {

constexpr int kScanMs = 12000;
constexpr int kOfflineMs = 5000;
constexpr int kMaxPubIntervalMs = 1000;
constexpr int kSvcWaitMs = 8000;

using clock = std::chrono::steady_clock;

bool snap_changed(const N2BleSnapshot& a, const N2BleSnapshot& b)
{
    return a.online != b.online
        || a.n2_valid != b.n2_valid
        || a.state != b.state
        || a.seq != b.seq
        || (a.n2_valid && b.n2_valid && a.n2_percent != b.n2_percent);
}

} // namespace

N2BleClient& N2BleClient::instance()
{
    static N2BleClient s;
    return s;
}

N2BleClient::~N2BleClient()
{
    shutdown();
}

bool N2BleClient::init(StatusCb cb)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb_ = std::move(cb);
    }
    if (running_.load()) {
        return true;
    }
    if (!N2BleTransport::compiled_with_bluez()) {
        UPC_LOG_N2_BLE(Warn, "compiled without BlueZ/libdbus; N2 BLE idle");
    }
    running_.store(true);
    th_ = std::thread([this] { run(); });
    return true;
}

void N2BleClient::shutdown()
{
    if (!running_.exchange(false)) {
        if (th_.joinable()) {
            th_.join();
        }
        return;
    }
    if (th_.joinable()) {
        th_.join();
    }
}

N2BleSnapshot N2BleClient::snapshot() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return snap_;
}

void N2BleClient::publish_if_needed(const N2BleSnapshot& snap, bool force)
{
    const auto now = clock::now();
    bool send = force;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!send && snap_changed(snap_, snap)) {
            send = true;
        }
        if (!send && now - last_pub_ >= std::chrono::milliseconds(kMaxPubIntervalMs)) {
            send = true;
        }
        snap_ = snap;
        if (send) {
            last_pub_ = now;
        }
    }
    if (!send) {
        return;
    }
    StatusCb cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = cb_;
    }
    if (cb) {
        cb(snap);
    }
}

void N2BleClient::run()
{
    N2BleTransport tx;
    if (!N2BleTransport::compiled_with_bluez()) {
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        return;
    }

    int backoff_ms = 1000;
    bool warned_open = false;
    while (running_.load()) {
        if (!tx.open()) {
            if (!warned_open) {
                UPC_LOG_N2_BLE(Warn, "bluez open failed; retry");
                warned_open = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            continue;
        }
        warned_open = false;
        tx.start_discovery();
        N2BleTransport::Device dev;
        bool found = false;
        const auto scan_end = clock::now() + std::chrono::milliseconds(kScanMs);
        while (running_.load() && clock::now() < scan_end) {
            tx.poll(200);
            if (tx.find_n2_machine(&dev)) {
                found = true;
                break;
            }
        }
        tx.stop_discovery();
        if (!found) {
            UPC_LOG_N2_BLE(Debug, "scan timeout, no N2_Machine");
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, 15000);
            continue;
        }
        UPC_LOG_N2_BLE(Info, "found %s %s", dev.name.c_str(), dev.address.c_str());
        if (!tx.connect_device(dev)) {
            UPC_LOG_N2_BLE(Warn, "connect failed %s", dev.address.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, 15000);
            continue;
        }
        backoff_ms = 1000;
        const auto svc_end = clock::now() + std::chrono::milliseconds(kSvcWaitMs);
        while (running_.load() && clock::now() < svc_end
               && !tx.services_resolved(dev.path)) {
            tx.poll(200);
        }
        std::string char_path;
        if (!tx.find_n2_characteristic(dev.path, &char_path)) {
            UPC_LOG_N2_BLE(Warn, "char 0x1B9A not found");
            tx.disconnect_device(dev);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        (void)tx.start_notify(char_path);

        auto last_notify = clock::now();
        bool ever_notify = false;
        N2BleSnapshot session{};
        session.online = true;
        publish_if_needed(session, true);

        while (running_.load() && tx.device_connected(dev.path)) {
            tx.poll(200);
            std::string ascii;
            bool got = tx.take_notify_ascii(&ascii);
            if (!got) {
                const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                    clock::now() - last_notify).count();
                if (idle > 1200) {
                    got = tx.read_ascii(char_path, &ascii);
                }
            }
            if (got) {
                last_notify = clock::now();
                ever_notify = true;
                N2BleParsed parsed;
                if (parse_n2_ble_ascii(ascii, &parsed)) {
                    session.online = true;
                    session.n2_valid = parsed.n2_valid;
                    session.n2_percent = parsed.n2_percent;
                    session.state = parsed.state;
                    session.seq = parsed.seq;
                    session.ascii = ascii;
                    UPC_LOG_N2_BLE(Info, "ascii=%s valid=%d pct=%.2f state=%s seq=%u",
                                   ascii.c_str(), session.n2_valid ? 1 : 0,
                                   static_cast<double>(session.n2_percent),
                                   n2_ble_state_name(session.state), session.seq);
                    publish_if_needed(session, false);
                } else {
                    UPC_LOG_N2_BLE(Warn, "parse fail [%s]", ascii.c_str());
                }
            }
            const auto silence = std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - last_notify).count();
            if (ever_notify && silence >= kOfflineMs) {
                UPC_LOG_N2_BLE(Warn, "notify silent %lld ms -> offline",
                               static_cast<long long>(silence));
                break;
            }
        }
        session.online = false;
        session.n2_valid = false;
        publish_if_needed(session, true);
        tx.stop_notify(char_path);
        tx.disconnect_device(dev);
        tx.poll(100);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    }
    tx.close();
}

} // namespace upc::glavo
