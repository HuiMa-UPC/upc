#include "upc_contract.hpp"
#include <array>
#include <cstring>
#include <iostream>
#include <type_traits>

using namespace middleware;
constexpr std::array<MsgType, 6> types{{
    MsgType::GLAVO_Visual_Cabin_Session_Start_Ntf,
    MsgType::GLAVO_Visual_Cabin_Session_Stop_Ntf,
    MsgType::GLAVO_Visual_InnerDoor_Occlusion_Req,
    MsgType::APP_Visual_InnerDoor_Occlusion_Rsp,
    MsgType::GLAVO_Visual_Plate_Present_Req,
    MsgType::APP_Visual_Plate_Present_Rsp}};
constexpr bool reliable_ids() {
    for(std::size_t i = 0; i < types.size(); ++i)
        if(static_cast<uint32_t>(types[i]) != 0x1720u + i ||
           !is_ipc_reliable_msg_type(types[i])) return false;
    return true;
}
static_assert(reliable_ids(), "all six cabin messages must be reliable and keep their reviewed IDs");
static_assert(std::is_trivially_copyable_v<VisualCabinSessionPayload>);
static_assert(std::is_trivially_copyable_v<VisualCabinCheckReqPayload>);
static_assert(std::is_trivially_copyable_v<VisualCabinCheckRspPayload>);
static_assert(std::is_standard_layout_v<VisualCabinCheckReqPayload>);
static_assert(sizeof(VisualCabinSessionPayload) == 4);
static_assert(sizeof(VisualCabinCheckReqPayload) == 16);
static_assert(sizeof(VisualCabinCheckRspPayload) == 16);

int main() {
    // Test the existing native little-endian wire ABI, including reserved bytes.
    VisualCabinCheckReqPayload req{0x01020304, 7, 10000, 0x3f, 1, {0,0}};
    const unsigned char expected[] = {4,3,2,1, 7,0,0,0, 0x10,0x27,0,0, 0x3f,1,0,0};
    if(std::memcmp(&req, expected, sizeof(req)) != 0) return 1;
    VisualCabinCheckRspPayload rsp{};
    const unsigned char zero[16]{};
    if(std::memcmp(&rsp, zero, sizeof(rsp)) != 0) return 1;
    std::cout << "Cabin IPC IDs, reliability, sizes and wire layout passed\n";
}
