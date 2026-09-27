#include "cabin_visual_exchange.hpp"
#include "cabin_detector.hpp"
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace middleware;
using namespace upc::glavo;
using namespace std::chrono_literals;
#define CHECK(expr) do { if(!(expr)) throw std::runtime_error(#expr); } while(false)

template<class Payload>
static Message message(MsgType type, ModuleId sender, const Payload& payload)
{
    Message msg(type, sender, ModuleId::APP_GLAVO);
    msg.payload = MsgPayload::from_bytes(
        reinterpret_cast<const uint8_t*>(&payload), sizeof(payload));
    return msg;
}

static void shared_session_sequence()
{
    // The former separate counters both issued 1: Visual rejects the second Start.
    CHECK(!upc::visual::cabin_session_start_allowed(1, false, 1000, 1, 2000));
    std::atomic<uint32_t> sequence{1};
    uint32_t previous = 0;
    // Door -> plate absent -> door open -> door close -> plate recheck.
    for(unsigned round = 0; round != 5; ++round) {
        const uint32_t next = next_cabin_id(sequence);
        CHECK(upc::visual::cabin_session_start_allowed(previous, false, 1000, next, 2000));
        previous = next;
    }
    sequence.store(std::numeric_limits<uint32_t>::max());
    previous = next_cabin_id(sequence);
    CHECK(previous == std::numeric_limits<uint32_t>::max());
    const uint32_t wrapped = next_cabin_id(sequence);
    CHECK(wrapped == 1);
    CHECK(upc::visual::cabin_session_start_allowed(previous, false, 1000, wrapped, 2000));
}

static void reply_packet_validation()
{
    const auto type = MsgType::APP_Visual_Plate_Present_Rsp;
    VisualCabinCheckRspPayload wire{5, 9, 11, 2, {0, 0, 0}}, decoded{};
    const auto valid = message(type, ModuleId::APP_VISUAL, wire);
    CHECK(read_cabin_reply(valid, type, decoded));
    CHECK(decoded.result == 2);
    const auto door = message(MsgType::APP_Visual_InnerDoor_Occlusion_Rsp,
                              ModuleId::APP_VISUAL, wire);
    CHECK(read_cabin_reply(door, MsgType::APP_Visual_InnerDoor_Occlusion_Rsp, decoded));
    CHECK(!read_cabin_reply(door, type, decoded));
    for(const auto length : {size_t(0), sizeof(wire) - 1, sizeof(wire) + 1}) {
        auto bad = valid; bad.payload.length = length;
        CHECK(!read_cabin_reply(bad, type, decoded));
    }
    for(const auto kind : {MsgPayload::Type::Empty, MsgPayload::Type::Value}) {
        auto bad = valid; bad.payload.type = kind;
        CHECK(!read_cabin_reply(bad, type, decoded));
    }
    auto bad = valid; bad.sender_id = ModuleId::UI;
    CHECK(!read_cabin_reply(bad, type, decoded));
    bad = valid; bad.target_id = ModuleId::UI;
    CHECK(!read_cabin_reply(bad, type, decoded));
    for(unsigned index = 0; index != 3; ++index) {
        auto payload = wire; payload.reserved[index] = 1;
        CHECK(!read_cabin_reply(message(type, ModuleId::APP_VISUAL, payload), type, decoded));
    }
    auto payload = wire; payload.result = 3;
    CHECK(!read_cabin_reply(message(type, ModuleId::APP_VISUAL, payload), type, decoded));
    payload = wire; payload.session_id = 0;
    CHECK(!read_cabin_reply(message(type, ModuleId::APP_VISUAL, payload), type, decoded));
    payload = wire; payload.request_id = 0;
    CHECK(!read_cabin_reply(message(type, ModuleId::APP_VISUAL, payload), type, decoded));
}

static void confirmation_packet_validation()
{
    const auto type = MsgType::UI_Plate_Place_Confirm_Rsp;
    PlatePlaceConfirmRspPayload wire{};
    const auto valid = message(type, ModuleId::UI, wire);
    const auto confirm = read_plate_place_confirmation(valid);
    CHECK(confirm.has_value() && !*confirm);
    wire.cancel = 1;
    const auto cancel = read_plate_place_confirmation(message(type, ModuleId::UI, wire));
    CHECK(cancel.has_value() && *cancel);
    for(const auto length : {size_t(0), sizeof(wire) - 1, sizeof(wire) + 1}) {
        auto bad = valid; bad.payload.length = length;
        CHECK(!read_plate_place_confirmation(bad).has_value());
    }
    for(const auto kind : {MsgPayload::Type::Empty, MsgPayload::Type::Value}) {
        auto bad = valid; bad.payload.type = kind;
        CHECK(!read_plate_place_confirmation(bad).has_value());
    }
    auto bad = valid; bad.sender_id = ModuleId::APP_VISUAL;
    CHECK(!read_plate_place_confirmation(bad).has_value());
    bad = valid; bad.target_id = ModuleId::APP_VISUAL;
    CHECK(!read_plate_place_confirmation(bad).has_value());
    bad = valid; bad.type = MsgType::APP_Visual_Plate_Present_Rsp;
    CHECK(!read_plate_place_confirmation(bad).has_value());
    for(unsigned index = 0; index != 3; ++index) {
        wire = {}; wire.reserved[index] = 1;
        CHECK(!read_plate_place_confirmation(message(type, ModuleId::UI, wire)).has_value());
    }
    for(const auto invalid : {2, 255}) {
        wire = {}; wire.cancel = static_cast<uint8_t>(invalid);
        CHECK(!read_plate_place_confirmation(message(type, ModuleId::UI, wire)).has_value());
    }
}

static void response_handoff()
{
    CabinVisualExchange exchange;
    std::atomic<bool> aborted{false};
    uint8_t result = 99;
    VisualCabinCheckRspPayload rsp{1, 1, 1, 2, {0, 0, 0}};
    CHECK(!exchange.accept(rsp));
    const auto instant = exchange.request(1, 1, 100ms, aborted, [&]() {
        auto stale = rsp; stale.session_id = 2;
        CHECK(!exchange.accept(stale));
        stale = rsp; stale.request_id = 2;
        CHECK(!exchange.accept(stale));
        // Deterministically reproduce a callback before publish returns.
        CHECK(exchange.accept(rsp));
        auto duplicate = rsp; duplicate.result = 1;
        CHECK(!exchange.accept(duplicate));
        return true;
    }, result);
    CHECK(instant == CabinExchangeStatus::Reply && result == 2);
    CHECK(!exchange.accept(rsp));

    CHECK(exchange.request(1, 2, 1ms, aborted, []() { return true; }, result)
          == CabinExchangeStatus::Timeout);
    CHECK(result == 0);
    rsp.request_id = 2;
    CHECK(!exchange.accept(rsp));

    CHECK(exchange.request(1, 3, 100ms, aborted, []() { return false; }, result)
          == CabinExchangeStatus::PublishFailed);
    CHECK(result == 0);
    rsp.request_id = 3;
    CHECK(!exchange.accept(rsp));

    aborted.store(true);
    bool published = false;
    CHECK(exchange.request(1, 4, 100ms, aborted, [&]() { published = true; return true; }, result)
          == CabinExchangeStatus::Aborted);
    CHECK(!published && result == 0);

    aborted.store(false);
    rsp.session_id = 2; rsp.request_id = 4;
    CHECK(exchange.request(2, 4, 100ms, aborted, [&]() {
        CHECK(exchange.accept(rsp));
        aborted.store(true);
        exchange.notify_abort();
        return true;
    }, result) == CabinExchangeStatus::Aborted);
    CHECK(result == 0); // Cancellation wins even over an already delivered positive.

    // Exercise callback-thread cancellation at the publisher/wait boundary.
    for(unsigned iteration = 0; iteration != 100; ++iteration) {
        aborted.store(false);
        std::promise<void> sent;
        auto ready = sent.get_future();
        std::thread cancel([&]() {
            ready.wait();
            aborted.store(true);
            exchange.notify_abort();
        });
        const auto started = std::chrono::steady_clock::now();
        const auto status = exchange.request(3, iteration + 1, 5s, aborted,
            [&]() { sent.set_value(); return true; }, result);
        cancel.join();
        CHECK(status == CabinExchangeStatus::Aborted && result == 0);
        CHECK(std::chrono::steady_clock::now() - started < 2s);
    }

    // A fresh round after timeout/cancellation can still consume its own reply.
    aborted.store(false);
    rsp.session_id = 4; rsp.request_id = 1;
    CHECK(exchange.request(4, 1, 100ms, aborted, [&]() {
        CHECK(exchange.accept(rsp)); return true;
    }, result) == CabinExchangeStatus::Reply);
    CHECK(result == 2);
}

int main()
{
    try {
        shared_session_sequence();
        reply_packet_validation();
        confirmation_packet_validation();
        response_handoff();
        std::cout << "SOC cabin sequence, response handoff, timeout/cancel and packet validation passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
