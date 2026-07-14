#ifndef UPC_IPC_REG_HPP
#define UPC_IPC_REG_HPP

#include "upc_contract.hpp"
#include "msg_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace upc_ipc {

/** client → broker：注册 ModuleId（length-prefix 帧 body） */
struct IpcRegFrame {
    uint32_t magic;
    uint32_t version;
    uint32_t module_id;
};

/** broker → client：peer 上下线通知（length-prefix 帧 body） */
struct IpcRegNotify {
    uint32_t magic;
    uint32_t version;
    uint32_t event;
    uint32_t module_id;
};

bool ipc_reg_encode(middleware::ModuleId module_id, std::vector<uint8_t>& out);
bool ipc_reg_decode(const uint8_t* data, size_t len, middleware::ModuleId& out);

bool ipc_reg_notify_encode(uint32_t event, middleware::ModuleId module_id,
                           std::vector<uint8_t>& out);
bool ipc_reg_notify_decode(const uint8_t* data, size_t len, uint32_t& event,
                           middleware::ModuleId& module_id);

/** 是否为 reg 或 reg-notify 控制帧（非 WIRE v1 业务） */
bool ipc_reg_is_control_frame(const uint8_t* data, size_t len);

} // namespace upc_ipc

#endif
