#include "input_snapshots.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <limits>

namespace ls_native {
using Microsoft::WRL::ComPtr;
namespace {
std::atomic<uint64_t> next_pool{0};
uint64_t LuidBits(LUID value) {
    return (uint64_t(static_cast<uint32_t>(value.HighPart)) << 32) | value.LowPart;
}
bool SameObject(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> ia, ib;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&ia))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&ib))) && ia.Get() == ib.Get();
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
bool Supported(DXGI_FORMAT format) {
    // Initial SDR transport: no implicit typeless/sRGB/FP16/color conversions.
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
}
} // namespace
struct InputSnapshots::State {
    enum class Phase { Free, Captured, Reading, Discarded };
    struct Slot {
        ComPtr<ID3D12Resource> texture12;
        ComPtr<ID3D11Texture2D> texture11;
        SnapshotTicket ticket;
        uint64_t reader_signal = 0;
        Phase phase = Phase::Free;
    };
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> producer12, reader;
    ComPtr<ID3D11Fence> producer11;
    std::array<Slot, 8> slots;
    uint32_t count = 0, width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t pool = 0, epoch = 0, luid = 0, serial = 0, read_signal = 0, last_write = 0;
    bool failed = false;
    bool Alive() {
        if (failed || FAILED(device11->GetDeviceRemovedReason()) ||
            FAILED(device12->GetDeviceRemovedReason())) { failed = true; return false; }
        return true;
    }
    Slot* Find(const SnapshotTicket& t) {
        if (t.pool != pool || t.epoch != epoch || !t.serial || t.slot >= count) return nullptr;
        auto& s = slots[t.slot];
        if (s.phase != Phase::Captured || s.ticket.serial != t.serial ||
            s.ticket.producer_fence != t.producer_fence ||
            s.ticket.source.source_object != t.source.source_object ||
            s.ticket.source.write_submission != t.source.write_submission) return nullptr;
        return &s;
    }
};
InputSnapshots::InputSnapshots() = default;
InputSnapshots::~InputSnapshots() {
    if (state_ && CloseIfIdle() != SnapshotResult::Ok) {
        OutputDebugStringW(L"NativeDLSS: retaining unfinished snapshot resources until process exit\n");
        // D3D12 command submissions do not keep resource objects alive for us.
        // There is deliberately no implicit wait/Flush in destruction.
        (void)state_.release();
    }
}
HRESULT InputSnapshots::Initialize(ID3D11DeviceContext* context, ID3D12Device* backend,
    ID3D12CommandQueue* queue, uint64_t epoch, uint32_t width, uint32_t height,
    DXGI_FORMAT format, uint32_t count) {
    if (state_ || !context || !backend || !queue || !epoch || epoch <= last_epoch_ || !width || !height ||
        width > 16384 || height > 16384 || count < 2 || count > 8 || !Supported(format) ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;
    auto s = std::make_unique<State>();
    ComPtr<ID3D11Device> native_device;
    context->GetDevice(&native_device);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> queue_device;
    DXGI_ADAPTER_DESC adapter_desc{};
    HRESULT hr = native_device.As(&s->device11);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = context->QueryInterface(IID_PPV_ARGS(&s->context)))) return hr;
    if (FAILED(hr = native_device.As(&dxgi))) return hr;
    if (FAILED(hr = dxgi->GetAdapter(&adapter))) return hr;
    if (FAILED(hr = adapter->GetDesc(&adapter_desc))) return hr;
    if (FAILED(hr = queue->GetDevice(IID_PPV_ARGS(&queue_device)))) return hr;
    s->luid = LuidBits(adapter_desc.AdapterLuid);
    if (!SameObject(queue_device.Get(), backend) ||
        s->luid != LuidBits(backend->GetAdapterLuid()) ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    s->device12 = backend; s->queue = queue;
    s->count = count; s->epoch = epoch; s->width = width; s->height = height; s->format = format;
    if (FAILED(hr = backend->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s->producer12)))) return hr;
    if (FAILED(hr = backend->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->reader)))) return hr;
    {
        Handle handle;
        if (FAILED(hr = backend->CreateSharedHandle(s->producer12.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value))) return hr;
        if (FAILED(hr = s->device11->OpenSharedFence(handle.value, IID_PPV_ARGS(&s->producer11)))) return hr;
    }
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    for (uint32_t i = 0; i < count; ++i) {
        auto& slot = s->slots[i];
        if (FAILED(hr = backend->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.texture12)))) return hr;
        Handle handle;
        if (FAILED(hr = backend->CreateSharedHandle(slot.texture12.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value))) return hr;
        if (FAILED(hr = s->device11->OpenSharedResource1(handle.value, IID_PPV_ARGS(&slot.texture11)))) return hr;
    }
    s->pool = ++next_pool;
    if (!s->pool) return E_FAIL;
    last_epoch_ = epoch;
    state_ = std::move(s);
    return S_OK;
}
SnapshotResult InputSnapshots::Collect() {
    if (!state_) return SnapshotResult::NotInitialized;
    auto& s = *state_;
    if (!s.Alive()) return SnapshotResult::DeviceFailure;
    const auto produced = s.producer12->GetCompletedValue(), read = s.reader->GetCompletedValue();
    if (produced == UINT64_MAX || read == UINT64_MAX) { s.failed = true; return SnapshotResult::DeviceFailure; }
    for (uint32_t i = 0; i < s.count; ++i) {
        auto& slot = s.slots[i];
        if ((slot.phase == State::Phase::Reading && read >= slot.reader_signal) ||
            (slot.phase == State::Phase::Discarded && produced >= slot.ticket.producer_fence))
            slot.phase = State::Phase::Free;
    }
    return SnapshotResult::Ok;
}
SnapshotResult InputSnapshots::Capture(ID3D11Texture2D* source, SourceSubmission submission,
    SnapshotTicket& out) {
    out = {};
    if (!state_) return SnapshotResult::NotInitialized;
    auto& s = *state_;
    if (!source || !submission.source_object || !submission.write_submission ||
        submission.write_submission <= s.last_write) return SnapshotResult::Invalid;
    if (Collect() != SnapshotResult::Ok) return SnapshotResult::DeviceFailure;
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
    if (!SameObject(device.Get(), s.device11.Get()) || desc.Width != s.width ||
        desc.Height != s.height || desc.Format != s.format || desc.MipLevels != 1 ||
        desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
        desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0)
        return SnapshotResult::Invalid;
    // No internally-owned snapshot may be presented back as a new input.
    for (uint32_t i = 0; i < s.count; ++i)
        if (SameObject(source, s.slots[i].texture11.Get())) return SnapshotResult::Invalid;
    State::Slot* slot = nullptr;
    uint32_t index = 0;
    for (; index < s.count; ++index) if (s.slots[index].phase == State::Phase::Free) { slot = &s.slots[index]; break; }
    if (!slot) return SnapshotResult::Busy;
    if (s.serial == UINT64_MAX - 1) { s.failed = true; return SnapshotResult::DeviceFailure; }
    ++s.serial;
    slot->ticket = {s.pool, s.epoch, s.serial, s.serial, submission, index};
    slot->phase = State::Phase::Captured;
    s.context->CopyResource(slot->texture11.Get(), source);
    if (FAILED(s.context->Signal(s.producer11.Get(), s.serial))) {
        s.failed = true; return SnapshotResult::DeviceFailure;
    }
    s.last_write = submission.write_submission;
    out = slot->ticket;
    return SnapshotResult::Ok;
}
ID3D12Resource* InputSnapshots::Resource(const SnapshotTicket& ticket) const {
    if (!state_ || state_->failed) return nullptr;
    auto* slot = state_->Find(ticket);
    return slot ? slot->texture12.Get() : nullptr;
}
SnapshotResult InputSnapshots::SubmitRead(const SnapshotTicket& ticket, ID3D12CommandList* commands) {
    return SubmitReads(&ticket, 1, commands);
}
SnapshotResult InputSnapshots::SubmitReads(const SnapshotTicket* tickets, uint32_t count,
    ID3D12CommandList* commands) {
    if (!state_) return SnapshotResult::NotInitialized;
    auto& s = *state_;
    if (!s.Alive()) return SnapshotResult::DeviceFailure;
    if (!tickets || !count || count > s.count) return SnapshotResult::Invalid;
    std::array<State::Slot*, 8> selected{};
    for (uint32_t i = 0; i < count; ++i) {
        selected[i] = s.Find(tickets[i]);
        if (!selected[i]) return SnapshotResult::Stale;
        for (uint32_t j = 0; j < i; ++j)
            if (selected[j] == selected[i]) return SnapshotResult::Invalid;
    }
    ComPtr<ID3D12Device> device;
    if (!commands || commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        FAILED(commands->GetDevice(IID_PPV_ARGS(&device))) || !SameObject(device.Get(), s.device12.Get()))
        return SnapshotResult::Invalid;
    if (s.read_signal == UINT64_MAX - 1) { s.failed = true; return SnapshotResult::DeviceFailure; }
    // GPU wait only. The caller must NOT flush native capture or wait on CPU.
    for (uint32_t i = 0; i < count; ++i)
        if (FAILED(s.queue->Wait(s.producer12.Get(), tickets[i].producer_fence))) {
            s.failed = true; return SnapshotResult::DeviceFailure;
        }
    s.queue->ExecuteCommandLists(1, &commands);
    ++s.read_signal;
    for (uint32_t i = 0; i < count; ++i) {
        selected[i]->reader_signal = s.read_signal;
        selected[i]->phase = State::Phase::Reading;
    }
    if (FAILED(s.queue->Signal(s.reader.Get(), s.read_signal))) {
        s.failed = true; return SnapshotResult::DeviceFailure;
    }
    return SnapshotResult::Ok;
}
SnapshotResult InputSnapshots::Discard(const SnapshotTicket& ticket) {
    if (!state_) return SnapshotResult::NotInitialized;
    if (!state_->Alive()) return SnapshotResult::DeviceFailure;
    auto* slot = state_->Find(ticket);
    if (!slot) return SnapshotResult::Stale;
    slot->phase = State::Phase::Discarded;
    return SnapshotResult::Ok;
}
SnapshotResult InputSnapshots::CloseIfIdle() {
    if (!state_) return SnapshotResult::Ok;
    if (Collect() != SnapshotResult::Ok) return SnapshotResult::DeviceFailure;
    for (uint32_t i = 0; i < state_->count; ++i)
        if (state_->slots[i].phase != State::Phase::Free) return SnapshotResult::Busy;
    state_.reset(); return SnapshotResult::Ok;
}
uint64_t InputSnapshots::AdapterLuid() const { return state_ ? state_->luid : 0; }
uint64_t InputSnapshots::LastReaderSignal() const { return state_ ? state_->read_signal : 0; }
ID3D12Fence* InputSnapshots::ReaderFence() const { return state_ ? state_->reader.Get() : nullptr; }
} // namespace ls_native
