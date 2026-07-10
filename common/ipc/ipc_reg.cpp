#include "ipc_reg.hpp"

#include <cstring>

namespace upc_ipc {

bool ipc_reg_encode(middleware::ModuleId module_id, std::vector<uint8_t>& out)
{
    IpcRegFrame frame{};
    frame.magic     = IPC_REG_MAGIC;
    frame.version   = IPC_REG_VERSION;
    frame.module_id = static_cast<uint32_t>(module_id);
    out.resize(sizeof(frame));
    std::memcpy(out.data(), &frame, sizeof(frame));
    return true;
}

bool ipc_reg_decode(const uint8_t* data, size_t len, middleware::ModuleId& out)
{
    if (!data || len < sizeof(IpcRegFrame)) {
        return false;
    }
    IpcRegFrame frame{};
    std::memcpy(&frame, data, sizeof(frame));
    if (frame.magic != IPC_REG_MAGIC || frame.version != IPC_REG_VERSION ||
        frame.module_id == 0) {
        return false;
    }
    out = static_cast<middleware::ModuleId>(frame.module_id);
    return true;
}

bool ipc_reg_notify_encode(uint32_t event, middleware::ModuleId module_id,
                           std::vector<uint8_t>& out)
{
    IpcRegNotify notify{};
    notify.magic     = IPC_REG_NOTIFY_MAGIC;
    notify.version   = IPC_REG_VERSION;
    notify.event     = event;
    notify.module_id = static_cast<uint32_t>(module_id);
    out.resize(sizeof(notify));
    std::memcpy(out.data(), &notify, sizeof(notify));
    return true;
}

bool ipc_reg_notify_decode(const uint8_t* data, size_t len, uint32_t& event,
                           middleware::ModuleId& module_id)
{
    if (!data || len < sizeof(IpcRegNotify)) {
        return false;
    }
    IpcRegNotify notify{};
    std::memcpy(&notify, data, sizeof(notify));
    if (notify.magic != IPC_REG_NOTIFY_MAGIC ||
        notify.version != IPC_REG_VERSION) {
        return false;
    }
    if (notify.event != IPC_REG_EVENT_ONLINE &&
        notify.event != IPC_REG_EVENT_OFFLINE) {
        return false;
    }
    if (notify.module_id == 0) {
        return false;
    }
    event     = notify.event;
    module_id = static_cast<middleware::ModuleId>(notify.module_id);
    return true;
}

bool ipc_reg_is_control_frame(const uint8_t* data, size_t len)
{
    if (!data || len < sizeof(uint32_t)) {
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, data, sizeof(magic));
    return magic == IPC_REG_MAGIC || magic == IPC_REG_NOTIFY_MAGIC;
}

} // namespace upc_ipc
