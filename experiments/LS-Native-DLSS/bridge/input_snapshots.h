#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <cstdint>
#include <memory>

namespace ls_native {
// Describes an observed, ordered write submission, NOT a game frame or GPU
// completion. The capture hook must supply this; texture pointers cannot do so.
struct SourceSubmission {
    uint64_t source_object = 0;
    uint64_t write_submission = 0;
};
struct SnapshotTicket {
    uint64_t pool = 0, epoch = 0, serial = 0, producer_fence = 0;
    SourceSubmission source;
    uint32_t slot = 0;
};
enum class SnapshotResult {
    Ok, Invalid, Busy, Stale, DeviceFailure, NotInitialized
};

// Experimental transport only: no LS hooks, NGX, NVOF, Present or CPU waits.
// All calls must be serialized with the LS immediate-context owner. Allocations
// happen in Initialize, outside render callbacks. No Flush is performed: the
// host's normal D3D11 submission determines when the producer signal progresses.
class InputSnapshots {
public:
    InputSnapshots();
    ~InputSnapshots();
    InputSnapshots(const InputSnapshots&) = delete;
    InputSnapshots& operator=(const InputSnapshots&) = delete;
    HRESULT Initialize(ID3D11DeviceContext* native_context, ID3D12Device* backend,
                       ID3D12CommandQueue* queue, uint64_t epoch, uint32_t width,
                       uint32_t height, DXGI_FORMAT format, uint32_t slots = 3);
    SnapshotResult Capture(ID3D11Texture2D* source, SourceSubmission submission,
                           SnapshotTicket& ticket);
    // Borrowed resource, valid until SubmitRead/Discard. Caller records read-only
    // commands with COMMON entry/exit states. Never submit those commands itself.
    ID3D12Resource* Resource(const SnapshotTicket& ticket) const;
    SnapshotResult SubmitRead(const SnapshotTicket& ticket, ID3D12CommandList* closed_commands);
    SnapshotResult SubmitReads(const SnapshotTicket* tickets, uint32_t count,
                               ID3D12CommandList* closed_commands);
    // Call BEFORE submitting to an independent engine such as NVOFA. It must
    // wait on ProducerFence/ticket.producer_fence and signal external_done only
    // after all reads finish. On uncertain API failure, retain these leases.
    SnapshotResult PinExternalReads(const SnapshotTicket* tickets, uint32_t count,
                                   ID3D12Fence* external_done, uint64_t value);
    SnapshotResult Discard(const SnapshotTicket& ticket);
    SnapshotResult Collect();
    // Nonblocking. Returns Busy while any lease or submitted GPU work exists.
    // Failure paths retain the graph for process lifetime rather than freeing
    // resources that a queue might still access. No destructor wait on LS.
    SnapshotResult CloseIfIdle();
    uint64_t AdapterLuid() const;
    uint64_t LastReaderSignal() const;
    ID3D12Fence* ReaderFence() const; // Borrowed; worker/test completion only.
    ID3D12CommandQueue* BackendQueue() const; // Borrowed; same-queue output retirement.
    ID3D12Fence* ProducerFence() const; // Borrowed; independent-engine dependency.
    const char* InitializationStep() const { return init_step_; }
private:
    struct State;
    std::unique_ptr<State> state_;
    uint64_t last_epoch_ = 0;
    const char* init_step_ = "not started";
};
} // namespace ls_native
