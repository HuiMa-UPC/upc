#ifndef UPC_IPC_WIRE_HPP
#define UPC_IPC_WIRE_HPP

#include "msg_types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace upc_ipc {

constexpr uint32_t WIRE_MAGIC   = 0x31504355u;
constexpr uint32_t WIRE_VERSION = 1u;
constexpr size_t WIRE_MAX_PAYLOAD = 65536u;

enum class WirePayloadType : uint8_t {
    Empty  = 0,
    Inline = 1,
    Value  = 3,
};

struct WireHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint8_t  priority;
    uint8_t  pad[3];
    uint32_t sender_id;
    uint32_t target_id;
    uint32_t timestamp;
    uint8_t  payload_type;
    uint8_t  pad2[3];
    uint32_t payload_length;
};

bool wire_encode(const middleware::Message& msg, std::vector<uint8_t>& out);
bool wire_decode(const uint8_t* data, size_t len, middleware::Message& out);

} // namespace upc_ipc

#endif
