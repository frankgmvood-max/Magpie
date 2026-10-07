#include "output_image.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
namespace ls_native {
using Microsoft::WRL::ComPtr;
namespace {
bool Same(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> x, y;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x.Get() == y.Get();
}
uint64_t Bits(LUID luid) {
    return (uint64_t(static_cast<uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
}
}
struct OutputImage::State {
    enum class Phase { Free, Acquired, Submitted, Published, Discarded, Copied };
    Phase phase = Phase::Free;
    Identity identity;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Texture2D> image11;
    ComPtr<ID3D12Resource> image12;
    ComPtr<ID3D12Fence> ready;
    ComPtr<ID3D11Fence> copied;
    uint64_t epoch = 0, luid = 0, ready_value = 0, copy_value = 0, last_slot = 0;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool failed = false, succeeded = false, disabled = true;
    bool Alive() {
        if (failed || FAILED(device11->GetDeviceRemovedReason()) || FAILED(device12->GetDeviceRemovedReason())) {
            failed = true; return false;
        }
        return true;
    }
};
OutputImage::OutputImage() = default;
OutputImage::~OutputImage() {
    if (state_ && CloseIfIdle() != SnapshotResult::Ok) {
        OutputDebugStringW(L"NativeDLSS: retaining unfinished output lease until process exit\n");
        (void)state_.release();
    }
}
HRESULT OutputImage::Initialize(ID3D11DeviceContext* context, ID3D12Device* backend,
    ID3D12CommandQueue* queue, uint64_t epoch, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (state_ || !context || !backend || !queue || !epoch || epoch <= last_epoch_ ||
        !width || !height || width > 16384 || height > 16384 ||
        (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;
    auto s = std::make_unique<State>();
    ComPtr<ID3D11Device> native;
    context->GetDevice(&native);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> queue_device;
    DXGI_ADAPTER_DESC ad{};
    HRESULT hr = native.As(&s->device11);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = context->QueryInterface(IID_PPV_ARGS(&s->context)))) return hr;
    if (FAILED(hr = native.As(&dxgi))) return hr;
    if (FAILED(hr = dxgi->GetAdapter(&adapter))) return hr;
    if (FAILED(hr = adapter->GetDesc(&ad))) return hr;
    if (FAILED(hr = queue->GetDevice(IID_PPV_ARGS(&queue_device)))) return hr;
    s->luid = Bits(ad.AdapterLuid);
    if (!Same(queue_device.Get(), backend) || s->luid != Bits(backend->GetAdapterLuid()) ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return E_INVALIDARG;
    s->device12 = backend; s->queue = queue; s->epoch = epoch;
    s->width = width; s->height = height; s->format = format;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(hr = s->device11->CreateTexture2D(&desc, nullptr, &s->image11))) return hr;
    ComPtr<IDXGIResource1> dxgi_resource;
    if (FAILED(hr = s->image11.As(&dxgi_resource))) return hr;
    HANDLE handle = nullptr;
    hr = dxgi_resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
        nullptr, &handle);
    if (FAILED(hr)) return hr;
    hr = backend->OpenSharedHandle(handle, IID_PPV_ARGS(&s->image12));
    CloseHandle(handle);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = backend->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->ready)))) return hr;
    if (FAILED(hr = s->device11->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->copied)))) return hr;
    last_epoch_ = epoch; state_ = std::move(s);
    return S_OK;
}
SnapshotResult OutputImage::Collect() {
    if (!state_) return SnapshotResult::NotInitialized;
    auto& s = *state_;
    if (!s.Alive()) return SnapshotResult::DeviceFailure;
    const auto ready = s.ready->GetCompletedValue(), copied = s.copied->GetCompletedValue();
    if (ready == UINT64_MAX || copied == UINT64_MAX) { s.failed = true; return SnapshotResult::DeviceFailure; }
    if ((s.phase == State::Phase::Discarded && ready >= s.ready_value) ||
        (s.phase == State::Phase::Copied && copied >= s.copy_value)) s.phase = State::Phase::Free;
    return SnapshotResult::Ok;
}
SnapshotResult OutputImage::Acquire(const Identity& identity) {
    if (!state_) return SnapshotResult::NotInitialized;
    auto& s = *state_;
    if (Collect() != SnapshotResult::Ok) return SnapshotResult::DeviceFailure;
    if (identity.device_epoch != s.epoch || identity.adapter_luid != s.luid ||
        identity.width != s.width || identity.height != s.height || identity.format != static_cast<uint32_t>(s.format) ||
        identity.color_space != 0 || identity.phase_bits != 0x3f000000u ||
        !identity.previous_frame || identity.previous_frame == UINT64_MAX ||
        identity.current_frame != identity.previous_frame + 1 || !identity.slot_sequence ||
        identity.slot_sequence <= s.last_slot) return SnapshotResult::Invalid;
    if (s.phase != State::Phase::Free) return SnapshotResult::Busy;
    s.identity = identity; s.last_slot = identity.slot_sequence;
    s.succeeded = false; s.disabled = true; s.phase = State::Phase::Acquired;
    return SnapshotResult::Ok;
}
ID3D12Resource* OutputImage::Resource(const Identity& identity) const {
    return state_ && !state_->failed && state_->phase == State::Phase::Acquired &&
        state_->identity == identity ? state_->image12.Get() : nullptr;
}
SnapshotResult OutputImage::Submit(const Identity& identity, InputSnapshots& inputs,
    const SnapshotTicket* tickets, uint32_t count, ID3D12CommandList* commands) {
    if (!state_) return SnapshotResult::NotInitialized;
    auto& s = *state_;
    if (!s.Alive()) return SnapshotResult::DeviceFailure;
    if (s.phase != State::Phase::Acquired || !(s.identity == identity)) return SnapshotResult::Stale;
    if (!tickets || !count || count > 8 || !Same(inputs.BackendQueue(), s.queue.Get()) ||
        !Same(inputs.NativeDevice(), s.device11.Get())) return SnapshotResult::Invalid;
    for (uint32_t i = 0; i < count; ++i) {
        if (tickets[i].epoch != s.epoch) return SnapshotResult::Invalid;
        auto* resource = inputs.Resource(tickets[i]);
        if (!resource) return SnapshotResult::Stale;
        const auto desc = resource->GetDesc();
        if (desc.Width != s.width || desc.Height != s.height || desc.Format != s.format)
            return SnapshotResult::Invalid;
    }
    if (s.ready_value == UINT64_MAX - 1) { s.failed = true; return SnapshotResult::DeviceFailure; }
    const auto result = inputs.SubmitReads(tickets, count, commands);
    if (result != SnapshotResult::Ok) {
        if (result == SnapshotResult::DeviceFailure) s.failed = true;
        return result;
    }
    s.phase = State::Phase::Submitted;
    if (FAILED(s.queue->Signal(s.ready.Get(), ++s.ready_value))) {
        s.failed = true; return SnapshotResult::DeviceFailure;
    }
    return SnapshotResult::Ok;
}
SnapshotResult OutputImage::PublishResult(const Identity& identity, bool succeeded, bool disabled) {
    if (!state_) return SnapshotResult::NotInitialized;
    auto& s = *state_;
    if (!s.Alive()) return SnapshotResult::DeviceFailure;
    if (s.phase != State::Phase::Submitted || !(s.identity == identity)) return SnapshotResult::Stale;
    const auto ready = s.ready->GetCompletedValue();
    if (ready == UINT64_MAX) { s.failed = true; return SnapshotResult::DeviceFailure; }
    if (ready < s.ready_value) return SnapshotResult::Busy;
    s.succeeded = succeeded; s.disabled = disabled; s.phase = State::Phase::Published;
    return SnapshotResult::Ok;
}
Decision OutputImage::TryCopy(const VerifiedContract& verified, const Identity& slot,
    ID3D11Texture2D* destination, uint64_t last_consumed_sequence) {
    if (!state_) return Decision::NativeFailure;
    auto& s = *state_;
    if (!s.Alive()) return Decision::NativeFailure;
    if (s.phase == State::Phase::Copied) return Decision::NativeAlreadyConsumed;
    CompletedImage candidate{s.identity, s.ready_value,
        s.phase == State::Phase::Published && s.succeeded, s.disabled,
        s.phase == State::Phase::Published};
    const auto decision = Choose(verified, slot, candidate, s.ready->GetCompletedValue(), last_consumed_sequence);
    if (decision != Decision::UseReplacement) return decision;
    if (!destination || Same(destination, s.image11.Get())) return Decision::NativeFailure;
    ComPtr<ID3D11Device> device; destination->GetDevice(&device);
    D3D11_TEXTURE2D_DESC desc{}; destination->GetDesc(&desc);
    if (!Same(device.Get(), s.device11.Get()) || desc.Width != s.width || desc.Height != s.height ||
        desc.Format != s.format || desc.MipLevels != 1 || desc.ArraySize != 1 ||
        desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality != 0 ||
        desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0) return Decision::NativeFailure;
    if (s.copy_value == UINT64_MAX - 1) { s.failed = true; return Decision::NativeFailure; }
    // Host must establish the exact slot/binding ownership contract before this
    // call. We do not mutate pipeline bindings, wait, Flush or Present.
    s.context->CopyResource(destination, s.image11.Get());
    s.phase = State::Phase::Copied;
    if (FAILED(s.context->Signal(s.copied.Get(), ++s.copy_value))) {
        s.failed = true; return Decision::NativeFailure;
    }
    return Decision::UseReplacement; // CPU submission only, not display completion.
}
SnapshotResult OutputImage::Discard() {
    if (!state_) return SnapshotResult::NotInitialized;
    auto& s = *state_;
    if (!s.Alive()) return SnapshotResult::DeviceFailure;
    switch (s.phase) {
    case State::Phase::Acquired: s.phase = State::Phase::Free; return SnapshotResult::Ok;
    case State::Phase::Submitted: case State::Phase::Published:
        s.phase = State::Phase::Discarded; return SnapshotResult::Ok;
    default: return SnapshotResult::Stale;
    }
}
SnapshotResult OutputImage::CloseIfIdle() {
    if (!state_) return SnapshotResult::Ok;
    if (Collect() != SnapshotResult::Ok) return SnapshotResult::DeviceFailure;
    if (state_->phase != State::Phase::Free) return SnapshotResult::Busy;
    state_.reset(); return SnapshotResult::Ok;
}
ID3D12Fence* OutputImage::ReadyFence() const { return state_ ? state_->ready.Get() : nullptr; }
uint64_t OutputImage::ReadySignal() const { return state_ ? state_->ready_value : 0; }
ID3D11Fence* OutputImage::CopyFence() const { return state_ ? state_->copied.Get() : nullptr; }
uint64_t OutputImage::CopySignal() const { return state_ ? state_->copy_value : 0; }
} // namespace ls_native
