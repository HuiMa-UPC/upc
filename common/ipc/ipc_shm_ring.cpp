#include "ipc_shm_ring.hpp"

#include <cstring>

namespace upc_ipc {

namespace {

uint32_t expected_state(IpcShmSlotState state)
{
    return static_cast<uint32_t>(state);
}

bool cas_slot_state(IpcShmSlotHeader& slot, IpcShmSlotState expected,
                    IpcShmSlotState desired)
{
    uint32_t exp = expected_state(expected);
    const uint32_t des = expected_state(desired);
    return slot.state.compare_exchange_strong(
        exp, des, std::memory_order_acq_rel, std::memory_order_acquire);
}

} // namespace

IpcShmRingProducer::~IpcShmRingProducer()
{
    destroy();
}

IpcShmError IpcShmRingProducer::create(const IpcShmRingCreateParams& params)
{
    destroy();
    layout_ = ipc_shm_compute_layout(params);
    if (layout_.total_bytes == 0 || !params.name) {
        return IpcShmError::InvalidArgument;
    }

    IpcShmError err = region_.create(params.name, layout_.total_bytes);
    if (err != IpcShmError::Ok) {
        return err;
    }

    std::memset(region_.data(), 0, layout_.total_bytes);

    hdr_ = reinterpret_cast<IpcShmGlobalHeader*>(region_.data());
    ipc_shm_fill_global_header(*hdr_, params, layout_);

    slots_ = reinterpret_cast<IpcShmSlotHeader*>(
        static_cast<uint8_t*>(region_.data()) + layout_.slots_offset);
    for (uint32_t i = 0; i < params.slot_count; ++i) {
        slots_[i].state.store(expected_state(IpcShmSlotState::Empty),
                              std::memory_order_relaxed);
        slots_[i].seq.store(0, std::memory_order_relaxed);
        slots_[i].timestamp_us = 0;
        slots_[i].reserved     = 0;
    }

    payload_base_ =
        static_cast<uint8_t*>(region_.data()) + layout_.payload_offset;

    params_ = params;
    active_ = true;
    return IpcShmError::Ok;
}

void IpcShmRingProducer::destroy()
{
    if (active_ && region_.is_creator()) {
        region_.unlink_name();
    }
    region_.close();
    hdr_          = nullptr;
    slots_        = nullptr;
    payload_base_ = nullptr;
    layout_       = {};
    params_       = {};
    active_       = false;
}

std::optional<uint32_t> IpcShmRingProducer::acquire_write_slot()
{
    if (!active_ || !hdr_ || !slots_) {
        return std::nullopt;
    }

    for (uint32_t i = 0; i < hdr_->slot_count; ++i) {
        if (cas_slot_state(slots_[i], IpcShmSlotState::Empty,
                           IpcShmSlotState::Writing)) {
            return i;
        }
    }
    return std::nullopt;
}

void IpcShmRingProducer::cancel_write_slot(uint32_t slot_index)
{
    if (!active_ || !hdr_ || slot_index >= hdr_->slot_count) {
        return;
    }
    slots_[slot_index].state.store(expected_state(IpcShmSlotState::Empty),
                                   std::memory_order_release);
}

uint32_t IpcShmRingProducer::commit_slot(uint32_t slot_index,
                                         uint64_t timestamp_us)
{
    if (!active_ || !hdr_ || slot_index >= hdr_->slot_count) {
        return 0;
    }

    IpcShmSlotHeader& slot = slots_[slot_index];
    slot.timestamp_us      = timestamp_us;
    const uint32_t next_seq =
        slot.seq.load(std::memory_order_relaxed) + 1u;
    slot.seq.store(next_seq, std::memory_order_release);
    slot.state.store(expected_state(IpcShmSlotState::Ready),
                     std::memory_order_release);
    return next_seq;
}

uint8_t* IpcShmRingProducer::slot_payload(uint32_t slot_index)
{
    if (!active_ || !hdr_ || !payload_base_) {
        return nullptr;
    }
    return ipc_shm_slot_payload_ptr(payload_base_, hdr_->slot_stride_bytes,
                                    slot_index);
}

IpcShmRingConsumer::~IpcShmRingConsumer()
{
    close();
}

IpcShmError IpcShmRingConsumer::open(const char* name)
{
    close();
    if (!name || !name[0]) {
        return IpcShmError::InvalidArgument;
    }

    IpcShmError err = region_.open(name, false);
    if (err != IpcShmError::Ok) {
        return err;
    }

    hdr_ = reinterpret_cast<IpcShmGlobalHeader*>(region_.data());
    if (!ipc_shm_validate_global_header(*hdr_)) {
        close();
        return IpcShmError::BadMagic;
    }

    const IpcShmLayout layout = ipc_shm_compute_layout(IpcShmRingCreateParams{
        name,
        hdr_->width,
        hdr_->height,
        hdr_->stride_y,
        hdr_->slot_count,
        static_cast<IpcShmPixelFormat>(hdr_->format),
    });

    slots_ = reinterpret_cast<IpcShmSlotHeader*>(
        static_cast<uint8_t*>(region_.data()) + layout.slots_offset);
    payload_base_ =
        static_cast<const uint8_t*>(region_.data()) + layout.payload_offset;

    last_seen_seq_ = 0;
    active_        = true;
    return IpcShmError::Ok;
}

void IpcShmRingConsumer::close()
{
    region_.close();
    hdr_           = nullptr;
    slots_         = nullptr;
    payload_base_  = nullptr;
    active_        = false;
    last_seen_seq_ = 0;
}

std::optional<IpcShmFrameView> IpcShmRingConsumer::acquire_latest_frame()
{
    if (!active_ || !hdr_ || !slots_ || !payload_base_) {
        return std::nullopt;
    }

    int best_idx = -1;
    uint32_t best_seq = last_seen_seq_;

    for (uint32_t i = 0; i < hdr_->slot_count; ++i) {
        const uint32_t state =
            slots_[i].state.load(std::memory_order_acquire);
        if (state != expected_state(IpcShmSlotState::Ready)) {
            continue;
        }
        const uint32_t seq = slots_[i].seq.load(std::memory_order_acquire);
        if (seq > best_seq) {
            best_seq = seq;
            best_idx = static_cast<int>(i);
        }
    }

    if (best_idx < 0) {
        return std::nullopt;
    }

    IpcShmSlotHeader& slot = slots_[static_cast<uint32_t>(best_idx)];
    if (!cas_slot_state(slot, IpcShmSlotState::Ready, IpcShmSlotState::Reading)) {
        return std::nullopt;
    }

    IpcShmFrameView view{};
    ipc_shm_frame_view_from_slot(*hdr_, payload_base_,
                               static_cast<uint32_t>(best_idx), slot, view);
    last_seen_seq_ = view.seq;
    return view;
}

void IpcShmRingConsumer::release_slot(uint32_t slot_index)
{
    if (!active_ || !hdr_ || slot_index >= hdr_->slot_count) {
        return;
    }
    slots_[slot_index].state.store(expected_state(IpcShmSlotState::Empty),
                                   std::memory_order_release);
}

} // namespace upc_ipc
