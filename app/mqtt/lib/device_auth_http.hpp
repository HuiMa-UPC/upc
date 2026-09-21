#ifndef UPC_DEVICE_AUTH_HTTP_HPP
#define UPC_DEVICE_AUTH_HTTP_HPP

#include <string>

namespace upc::mqtt {

/**
 * 设备端激活 / 凭证轮换共用的 HTTPS JSON POST 与 HMAC 工具。
 *
 * 每次 POST 自行生成 X-Timestamp（Unix 毫秒）和 X-Nonce（16 位小写 hex），
 * 与《设备端凭证轮换对接手册》§4.0 / 激活规范的防重放头一致。
 */
bool device_auth_post_json(const std::string& base_url,
                           const std::string& endpoint,
                           const std::string& body,
                           int& status,
                           std::string& response,
                           std::string& error);

bool device_auth_random_hex(size_t bytes, std::string& out, std::string& error);

/** HMAC-SHA256(key=SHA256(device_secret) 的 32 字节, data) → 64 位小写 hex。失败返回空串。 */
[[nodiscard]] std::string device_auth_hmac_sha256_hex(const std::string& device_secret,
                                                      const std::string& sign_data);

[[nodiscard]] std::string device_auth_json_escape(const std::string& value);

bool device_auth_json_extract_string(const std::string& json, const char* key, std::string& out);

bool device_auth_response_ok(int status, const std::string& response);

} // namespace upc::mqtt

#endif
