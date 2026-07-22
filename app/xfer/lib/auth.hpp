#ifndef UPC_XFR_AUTH_HPP
#define UPC_XFR_AUTH_HPP

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace upc::xfr {

struct PairStartResult {
    bool ok = false;
    std::string pair_session;
    std::string pin;
    uint32_t expires_in = 0;
    std::string err_code;
    std::string err_msg;
};

struct PairVerifyResult {
    bool ok = false;
    std::string token;
    std::string name;
    std::string err_code;
    std::string err_msg;
    int http_status = 400;
};

class PairManager {
public:
    using PinNotify = std::function<void(const std::string& pin, uint32_t expires_s)>;

    PairManager(std::string token_file, uint32_t ttl_s, uint32_t max_attempts);

    void set_pin_notify(PinNotify cb);
    bool load_tokens();
    bool persist_tokens();

    PairStartResult start_pair(const std::string& client_name);
    PairVerifyResult verify_pair(const std::string& session_id,
                                 const std::string& pin,
                                 const std::string& device_name,
                                 const std::string& model_fallback);

    bool validate_token(const std::string& token) const;
    bool revoke_token(const std::string& token);

private:
    struct PairSession {
        std::string pin;
        std::chrono::steady_clock::time_point expire_at;
        uint32_t failed_attempts = 0;
        std::string client_name;
    };

    std::string make_random_hex(std::size_t n) const;
    std::string make_pin() const;
    void cleanup_expired_locked();
    bool persist_tokens_locked();

    std::string token_file_;
    uint32_t ttl_s_ = 120;
    uint32_t max_attempts_ = 5;

    mutable std::mutex mu_;
    std::unordered_map<std::string, PairSession> sessions_;
    std::unordered_map<std::string, std::string> tokens_;
    PinNotify pin_notify_;
};

} // namespace upc::xfr

#endif
