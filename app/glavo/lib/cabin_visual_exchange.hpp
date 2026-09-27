#ifndef UPC_GLAVO_CABIN_VISUAL_EXCHANGE_HPP
#define UPC_GLAVO_CABIN_VISUAL_EXCHANGE_HPP

#include "msg_runtime.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>

namespace upc::glavo {

inline uint32_t next_cabin_id(std::atomic<uint32_t>& sequence)
{
    uint32_t id = sequence.fetch_add(1);
    if(id == 0u) id = sequence.fetch_add(1);
    return id;
}

inline bool read_cabin_reply(const middleware::Message& msg,
                             middleware::MsgType expected_type,
                             middleware::VisualCabinCheckRspPayload& rsp)
{
    if(msg.type != expected_type ||
       msg.sender_id != middleware::ModuleId::APP_VISUAL ||
       msg.target_id != middleware::ModuleId::APP_GLAVO ||
       msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length != sizeof(rsp)) return false;
    std::memcpy(&rsp, msg.payload.data.bytes.data(), sizeof(rsp));
    return rsp.session_id != 0u && rsp.request_id != 0u && rsp.result <= 2u &&
           rsp.reserved[0] == 0u && rsp.reserved[1] == 0u && rsp.reserved[2] == 0u;
}

inline std::optional<bool> read_plate_place_confirmation(const middleware::Message& msg)
{
    middleware::PlatePlaceConfirmRspPayload payload{};
    if(msg.type != middleware::MsgType::UI_Plate_Place_Confirm_Rsp ||
       msg.sender_id != middleware::ModuleId::UI ||
       msg.target_id != middleware::ModuleId::APP_GLAVO ||
       msg.payload.type != middleware::MsgPayload::Type::Inline ||
       msg.payload.length != sizeof(payload)) return std::nullopt;
    std::memcpy(&payload, msg.payload.data.bytes.data(), sizeof(payload));
    if(payload.cancel > 1u || payload.reserved[0] != 0u ||
       payload.reserved[1] != 0u || payload.reserved[2] != 0u) return std::nullopt;
    return payload.cancel != 0u;
}

enum class CabinExchangeStatus { Reply, PublishFailed, Aborted, Timeout };

/** Hardware-independent reply handoff between the worker and IPC callback.
 * Arm before publishing: a fast/rejected request may receive a synchronous reply.
 * Each request has one terminal result; late/duplicate replies cannot replace it. */
class CabinVisualExchange {
public:
    template<class Publish>
    CabinExchangeStatus request(uint32_t session_id, uint32_t request_id,
                                std::chrono::milliseconds budget,
                                const std::atomic<bool>& aborted,
                                Publish publish, uint8_t& result)
    {
        result = 0; // Both cabin result enums define Unknown as zero.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if(aborted.load()) return CabinExchangeStatus::Aborted;
            session_id_ = session_id;
            request_id_ = request_id;
            arrived_ = false;
            waiting_ = true;
        }
        if(!publish()) {
            std::lock_guard<std::mutex> lock(mutex_);
            waiting_ = false;
            return CabinExchangeStatus::PublishFailed;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, budget, [&]() { return arrived_ || aborted.load(); });
        waiting_ = false;
        if(aborted.load()) return CabinExchangeStatus::Aborted;
        if(!arrived_) return CabinExchangeStatus::Timeout;
        result = result_;
        return CabinExchangeStatus::Reply;
    }

    bool accept(const middleware::VisualCabinCheckRspPayload& rsp)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!waiting_ || arrived_ || rsp.session_id != session_id_ ||
           rsp.request_id != request_id_) return false;
        result_ = rsp.result;
        arrived_ = true;
        cv_.notify_all();
        return true;
    }

    void notify_abort()
    {
        // Synchronize with the wait predicate so an abort wakeup cannot be lost.
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool waiting_ = false;
    bool arrived_ = false;
    uint32_t session_id_ = 0;
    uint32_t request_id_ = 0;
    uint8_t result_ = 0;
};

} // namespace upc::glavo
#endif
