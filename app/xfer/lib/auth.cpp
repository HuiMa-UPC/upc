#include "auth.hpp"

#include "rs_log.hpp"

#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace upc::xfr {

PairManager::PairManager(std::string token_file, uint32_t ttl_s, uint32_t max_attempts)
    : token_file_(std::move(token_file)),
      ttl_s_(ttl_s ? ttl_s : 120U),
      max_attempts_(max_attempts ? max_attempts : 5U)
{
}

void PairManager::set_pin_notify(PinNotify cb)
{
    std::lock_guard<std::mutex> lk(mu_);
    pin_notify_ = std::move(cb);
}

bool PairManager::load_tokens()
{
    std::lock_guard<std::mutex> lk(mu_);
    //清空缓存
    tokens_.clear();
    //打开 token_file_（默认 /data/transport/.xfr_tokens）
    std::ifstream ifs(token_file_);
    if (!ifs.good()) {
        return true;
    }
    std::string line;
    while (std::getline(ifs, line)) {
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string token = line.substr(0, tab);
        const std::string name = line.substr(tab + 1);
        if (!token.empty()) {
            tokens_[token] = name;
        }
    }
    return true;
}

bool PairManager::persist_tokens()
{
    std::lock_guard<std::mutex> lk(mu_);
    return persist_tokens_locked();
}

bool PairManager::persist_tokens_locked()
{
    std::ofstream ofs(token_file_, std::ios::trunc);
    if (!ofs.good()) {
        return false;
    }
    for (const auto& kv : tokens_) {
        ofs << kv.first << '\t' << kv.second << '\n';
    }
    return true;
}

PairStartResult PairManager::start_pair(const std::string& client_name)
{
    PairStartResult out;
    std::lock_guard<std::mutex> lk(mu_);
    cleanup_expired_locked();
    const std::string session = make_random_hex(24);
    const std::string pin = make_pin();
    PairSession s{};
    s.pin = pin;
    s.client_name = client_name;
    s.expire_at = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_s_);
    sessions_[session] = s;
    out.ok = true;
    out.pair_session = session;
    out.pin = pin;
    out.expires_in = ttl_s_;
    if (pin_notify_) {
        pin_notify_(pin, ttl_s_);
    }
    return out;
}

PairVerifyResult PairManager::verify_pair(const std::string& session_id,
                                          const std::string& pin,
                                          const std::string& device_name,
                                          const std::string& model_fallback)
{
    PairVerifyResult out;
    std::lock_guard<std::mutex> lk(mu_);
    cleanup_expired_locked();
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        out.err_code = "PAIR_EXPIRED";
        out.err_msg = "pair session expired";
        out.http_status = 410;
        return out;
    }

    PairSession& s = it->second;
    if (s.failed_attempts >= max_attempts_) {
        sessions_.erase(it);
        out.err_code = "PAIR_LOCKED";
        out.err_msg = "pair session locked";
        out.http_status = 429;
        return out;
    }
    if (pin != s.pin) {
        ++s.failed_attempts;
        out.err_code = "PAIR_BAD_PIN";
        out.err_msg = "invalid pin";
        out.http_status = 401;
        return out;
    }

    const std::string token = make_random_hex(40);
    const std::string name = device_name.empty() ? model_fallback : device_name;
    tokens_[token] = name;
    sessions_.erase(it);
    out.ok = true;
    out.token = token;
    out.name = name;
    if (!persist_tokens_locked()) {
        UPC_LOG_XFR_SERVICE(Warn, "persist token file failed: %s", token_file_.c_str());
    }
    return out;
}

bool PairManager::validate_token(const std::string& token) const
{
    std::lock_guard<std::mutex> lk(mu_);
    return tokens_.find(token) != tokens_.end();
}

bool PairManager::revoke_token(const std::string& token)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tokens_.find(token);
    if (it == tokens_.end()) {
        return false;
    }
    tokens_.erase(it);
    return persist_tokens_locked();
}

std::string PairManager::make_random_hex(std::size_t n) const
{
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(kHex[rng() & 0x0F]);
    }
    return out;
}

std::string PairManager::make_pin() const
{
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, 9999);
    const int v = dist(rng);
    char pin[8];
    std::snprintf(pin, sizeof(pin), "%04d", v);
    return pin;
}

void PairManager::cleanup_expired_locked()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (it->second.expire_at <= now) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace upc::xfr
