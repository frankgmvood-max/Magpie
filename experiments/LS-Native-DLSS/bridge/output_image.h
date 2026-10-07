#pragma once
#include "input_snapshots.h"
#include "../include/native_slot_policy.h"

namespace ls_native {
// One preallocated output lease. Host integration will own a bounded collection.
// Caller serializes every operation with native immediate-context ownership.
class OutputImage {
public:
    OutputImage();
    ~OutputImage();
    OutputImage(const OutputImage&) = delete;
    OutputImage& operator=(const OutputImage&) = delete;
    HRESULT Initialize(ID3D11DeviceContext* native_context, ID3D12Device* backend,
                       ID3D12CommandQueue* queue, uint64_t epoch, uint32_t width,
                       uint32_t height, DXGI_FORMAT format);
    SnapshotResult Acquire(const Identity& identity);
    ID3D12Resource* Resource(const Identity& identity) const; // Borrowed while acquired.
    // Reads and output writes are in ONE closed list on the same backend queue.
    // All resources must leave the command list in COMMON state.
    SnapshotResult Submit(const Identity& identity, InputSnapshots& inputs,
        const SnapshotTicket* tickets, uint32_t count, ID3D12CommandList* commands);
    // Worker supplies actual backend result + disable-interpolation flag AFTER
    // GPU completion/readback. Default is unknown/disabled; never assume success.
    SnapshotResult PublishResult(const Identity& identity, bool succeeded, bool interpolation_disabled);
    Decision TryCopy(const VerifiedContract& verified, const Identity& native_slot,
                     ID3D11Texture2D* destination, uint64_t last_consumed_sequence);
    SnapshotResult Discard();
    SnapshotResult Collect();
    SnapshotResult CloseIfIdle();
    ID3D12Fence* ReadyFence() const;
    uint64_t ReadySignal() const;
    ID3D11Fence* CopyFence() const;
    uint64_t CopySignal() const;
private:
    struct State;
    std::unique_ptr<State> state_;
    uint64_t last_epoch_ = 0;
};
} // namespace ls_native
