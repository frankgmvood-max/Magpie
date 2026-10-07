#include "../../bridge/input_snapshots.h"
#include "../../bridge/output_image.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
using namespace ls_native;
namespace {
void Check(bool success, const char* message) {
    if (!success) throw std::runtime_error(message);
}
void Hr(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << message << " HRESULT=0x" << std::hex << static_cast<uint32_t>(hr) << std::dec << '\n';
        throw std::runtime_error(message);
    }
}
// CPU waits, native Flush and readback exist in THIS TEST ONLY, never in the
// transport or a render hook. WARP cannot validate NVIDIA inference or cadence.
void Wait(ID3D12Fence* fence, uint64_t value) {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "create completion event");
    HRESULT hr = fence->SetEventOnCompletion(value, event);
    DWORD result = SUCCEEDED(hr) ? WaitForSingleObject(event, 15000) : WAIT_FAILED;
    CloseHandle(event);
    Hr(hr, "completion registration");
    Check(result == WAIT_OBJECT_0 && fence->GetCompletedValue() != UINT64_MAX,
          "bounded worker/test completion wait");
}
constexpr uint32_t kWidth = 19, kHeight = 7; // exercises readback row padding
std::vector<uint8_t> Pattern(uint8_t seed) {
    std::vector<uint8_t> bytes(kWidth * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y) for (uint32_t x = 0; x < kWidth; ++x) {
        const auto i = (y * kWidth + x) * 4;
        bytes[i] = static_cast<uint8_t>(x + seed);
        bytes[i+1] = static_cast<uint8_t>(y * 3 + seed);
        bytes[i+2] = seed; bytes[i+3] = 255;
    }
    return bytes;
}
struct Readback {
    ComPtr<ID3D12Resource> bytes;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    void Create(ID3D12Device* device, ID3D12Resource* source) {
        const auto desc = source->GetDesc();
        UINT64 total = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total; buffer.Height = 1; buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bytes)), "create readback");
    }
    void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* source) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = bytes.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list->ResourceBarrier(1, &barrier);
    }
    void Verify(const std::vector<uint8_t>& expected) {
        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(bytes->GetDesc().Width)};
        Hr(bytes->Map(0, &range, &mapped), "map completed readback");
        bool equal = true;
        const auto* raw = static_cast<const uint8_t*>(mapped) + footprint.Offset;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth * 4; ++x)
                equal &= raw[y * footprint.Footprint.RowPitch + x] == expected[y * kWidth * 4 + x];
        D3D12_RANGE written{0, 0}; bytes->Unmap(0, &written);
        Check(equal, "snapshot contents survived source reuse");
    }
};
void RoundTrip(InputSnapshots& inputs, const SnapshotTicket& input, ID3D11Device* d11,
    ID3D11DeviceContext* context, ID3D12Device* d12, ID3D12CommandQueue* queue,
    DXGI_FORMAT format, const std::vector<uint8_t>& expected) {
    OutputImage output;
    Hr(output.Initialize(context, d12, queue, 77, kWidth, kHeight, format), "initialize output image");
    Identity slot{77, inputs.AdapterLuid(), 2, 3, 22, kWidth, kHeight,
        static_cast<uint32_t>(format), 0, 0x3f000000u};
    Check(output.Acquire(slot) == SnapshotResult::Ok, "reserve exact output slot");
    auto old = slot; --old.slot_sequence;
    Check(!output.Resource(old), "output slot mismatch rejected");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Hr(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "output allocator");
    Hr(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(&list)), "output commands");
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& b : barriers) {
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    }
    barriers[0].Transition.pResource = inputs.Resource(input);
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource = output.Resource(slot);
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(2, barriers);
    // No-op backend establishes bit-exact transport. This is not DLSS inference.
    list->CopyResource(output.Resource(slot), inputs.Resource(input));
    for (auto& b : barriers) {
        b.Transition.StateBefore = b.Transition.StateAfter;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    }
    list->ResourceBarrier(2, barriers);
    Hr(list->Close(), "close output commands");
    ComPtr<ID3D12Fence> backend_gate;
    Hr(d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&backend_gate)), "output reader gate");
    Hr(queue->Wait(backend_gate.Get(), 1), "hold output work");
    auto changed_input = input; ++changed_input.epoch;
    Check(output.Submit(slot, inputs, &changed_input, 1, list.Get()) == SnapshotResult::Invalid,
        "another input epoch cannot populate this output slot");
    Check(output.Submit(slot, inputs, &input, 1, list.Get()) == SnapshotResult::Ok, "submit no-op backend");
    Check(output.PublishResult(slot, true, false) == SnapshotResult::Busy, "cannot publish unfinished GPU output");
    Check(output.CloseIfIdle() == SnapshotResult::Busy, "backend output retains allocation");
    Hr(backend_gate->Signal(1), "release backend output");
    context->Flush(); // Test's host submission boundary only.
    Wait(output.ReadyFence(), output.ReadySignal());

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth; desc.Height = kHeight; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> destination;
    Hr(d11->CreateTexture2D(&desc, nullptr, &destination), "native output destination");
    Check(output.TryCopy({true,true,true,true}, slot, destination.Get(), 0) == Decision::NativeFailure,
        "completed GPU output without a decision remains native");
    Check(output.PublishResult(slot, true, false) == SnapshotResult::Ok, "publish worker no-op result");
    Check(output.TryCopy({}, slot, destination.Get(), 0) == Decision::NativeUnverified, "unverified LS contract forbids copy");
    Check(output.TryCopy({true,true,true,true}, old, destination.Get(), 0) == Decision::NativeMismatch, "wrong exact slot forbids copy");

    // Independently hold the native copy to prove output lifetime extends past
    // D3D12 completion, all the way through D3D11 copy retirement.
    ComPtr<ID3D12Fence> native_gate;
    ComPtr<ID3D11Device5> d11v5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> gate11;
    Hr(d11->QueryInterface(IID_PPV_ARGS(&d11v5)), "output device5");
    Hr(context->QueryInterface(IID_PPV_ARGS(&context4)), "output context4");
    Hr(d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&native_gate)), "native copy gate");
    HANDLE handle = nullptr;
    Hr(d12->CreateSharedHandle(native_gate.Get(), nullptr, GENERIC_ALL, nullptr, &handle), "native gate handle");
    HRESULT open = d11v5->OpenSharedFence(handle, IID_PPV_ARGS(&gate11)); CloseHandle(handle);
    Hr(open, "import native copy gate");
    Hr(context4->Wait(gate11.Get(), 1), "hold native copy queue");
    Check(output.TryCopy({true,true,true,true}, slot, destination.Get(), 0) == Decision::UseReplacement, "submit exact native slot copy");
    Check(output.TryCopy({true,true,true,true}, slot, destination.Get(), 0) == Decision::NativeAlreadyConsumed, "output consumed once");
    auto next = slot; ++next.slot_sequence;
    Check(output.Acquire(next) == SnapshotResult::Busy, "output not reused at D3D12 completion");
    Check(output.CloseIfIdle() == SnapshotResult::Busy, "native copy retains output lease");
    Hr(native_gate->Signal(1), "release native copy");
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(done != nullptr, "native output completion event");
    Hr(output.CopyFence()->SetEventOnCompletion(output.CopySignal(), done), "output completion registration");
    context->Flush();
    DWORD wait = WaitForSingleObject(done, 15000); CloseHandle(done);
    Check(wait == WAIT_OBJECT_0, "native output copy retired");
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    Hr(d11->CreateTexture2D(&desc, nullptr, &readback), "native readback texture");
    context->CopyResource(readback.Get(), destination.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Hr(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "native readback TEST ONLY");
    bool equal = true;
    for (uint32_t y = 0; y < kHeight; ++y) for (uint32_t x = 0; x < kWidth * 4; ++x)
        equal &= static_cast<const uint8_t*>(mapped.pData)[y * mapped.RowPitch + x] == expected[y * kWidth * 4 + x];
    context->Unmap(readback.Get(), 0);
    Check(equal, "D3D11->D3D12->D3D11 round-trip is bit exact");
    Check(output.Acquire(next) == SnapshotResult::Ok, "output can recycle after native copy completion");
    Check(output.Discard() == SnapshotResult::Ok && output.CloseIfIdle() == SnapshotResult::Ok,
        "unused output reservation teardown");
    std::cout << "PASS output round-trip format=" << static_cast<uint32_t>(format)
        << ": exact image, ready-only copy, native copy retirement; no LS/Present test\n";
}
void Run(DXGI_FORMAT format) {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D12Device> d12;
    ComPtr<ID3D12CommandQueue> queue;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    Hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &d11, nullptr, &context), "D3D11 WARP");
    Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12)), "D3D12 WARP");
    D3D12_COMMAND_QUEUE_DESC qdesc{}; qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Hr(d12->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue)), "reader queue");
    InputSnapshots pool;
    Check(pool.Collect() == SnapshotResult::NotInitialized, "uninitialized gate");
    Check(pool.Initialize(context.Get(), d12.Get(), queue.Get(), 0, kWidth, kHeight, format, 2)
        == E_INVALIDARG, "zero epoch rejected");
    const HRESULT init = pool.Initialize(context.Get(), d12.Get(), queue.Get(), 77, kWidth, kHeight, format, 2);
    if (FAILED(init)) std::cerr << "Initialization stage: " << pool.InitializationStep() << '\n';
    Hr(init, "initialize shared snapshots");
    Check(pool.AdapterLuid() != 0, "actual adapter identity");
    Check(pool.Initialize(context.Get(), d12.Get(), queue.Get(), 78, kWidth, kHeight, format, 2)
        == E_INVALIDARG, "live epoch cannot be replaced");

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth; desc.Height = kHeight; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source;
    Hr(d11->CreateTexture2D(&desc, nullptr, &source), "source texture");
    const auto first = Pattern(10), second = Pattern(90), third = Pattern(170);
    std::array<SnapshotTicket, 2> tickets{};
    context->UpdateSubresource(source.Get(), 0, nullptr, first.data(), kWidth * 4, 0);
    Check(pool.Capture(source.Get(), {100, 1}, tickets[0]) == SnapshotResult::Ok, "capture first write");
    context->UpdateSubresource(source.Get(), 0, nullptr, second.data(), kWidth * 4, 0);
    Check(pool.Capture(source.Get(), {100, 2}, tickets[1]) == SnapshotResult::Ok, "capture second write on SAME texture");
    context->UpdateSubresource(source.Get(), 0, nullptr, third.data(), kWidth * 4, 0);
    SnapshotTicket extra;
    Check(pool.Capture(source.Get(), {100, 3}, extra) == SnapshotResult::Busy && !extra.serial, "bounded pool does not overwrite leases");
    Check(pool.Capture(source.Get(), {100, 2}, extra) == SnapshotResult::Invalid, "duplicate write is not another frame");
    auto forged = tickets[0]; ++forged.source.write_submission;
    Check(!pool.Resource(forged), "modified provenance rejected");
    forged = tickets[0]; ++forged.epoch;
    Check(pool.Discard(forged) == SnapshotResult::Stale, "old epoch rejected");
    Check(pool.CloseIfIdle() == SnapshotResult::Busy, "live capture prevents teardown");

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Hr(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "command allocator");
    Hr(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "readback commands");
    Readback a, b;
    a.Create(d12.Get(), pool.Resource(tickets[0])); b.Create(d12.Get(), pool.Resource(tickets[1]));
    a.Record(list.Get(), pool.Resource(tickets[0])); b.Record(list.Get(), pool.Resource(tickets[1]));
    Hr(list->Close(), "close read commands");
    const std::array<SnapshotTicket, 2> duplicate{tickets[0], tickets[0]};
    Check(pool.SubmitReads(duplicate.data(), 2, list.Get()) == SnapshotResult::Invalid, "duplicate lease rejected before GPU submit");
    // Simulate the independent NVOFA engine with a second D3D12 queue. Native
    // producer completion alone cannot retire its reads or admit NGX consumption.
    ComPtr<ID3D12CommandQueue> external_queue;
    ComPtr<ID3D12Fence> external_done, external_gate;
    Hr(d12->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&external_queue)), "independent reader queue");
    Hr(d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&external_done)), "independent reader completion");
    Hr(d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&external_gate)), "independent reader test gate");
    Check(pool.PinExternalReads(tickets.data(), 2, external_done.Get(), 0) == SnapshotResult::Invalid, "zero external signal rejected");
    Check(pool.PinExternalReads(tickets.data(), 2, pool.ReaderFence(), 1) == SnapshotResult::Invalid, "self-dependent reader timeline rejected");
    Check(pool.PinExternalReads(tickets.data(), 2, external_done.Get(), 1) == SnapshotResult::Ok, "pin pair before independent engine submission");
    Check(pool.PinExternalReads(tickets.data(), 2, external_done.Get(), 1) == SnapshotResult::Stale, "external read reservation is single use");
    Hr(external_queue->Wait(pool.ProducerFence(), tickets[1].producer_fence), "independent engine waits on captured pair");
    Hr(external_queue->Wait(external_gate.Get(), 1), "hold independent reader");
    Hr(external_queue->Signal(external_done.Get(), 1), "independent engine completion signal");
    // Hold the reader queue deliberately: completion of native capture alone
    // must NOT allow slot recycling while a consumer is still outstanding.
    ComPtr<ID3D12Fence> gate;
    Hr(d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "test queue gate");
    Hr(queue->Wait(gate.Get(), 1), "hold reader queue");
    Check(pool.SubmitReads(tickets.data(), 2, list.Get()) == SnapshotResult::Ok, "submit pair with both producer dependencies");
    context->Flush(); // Stand-in for host submission, NEVER done by the module.
    Check(pool.Capture(source.Get(), {100, 3}, extra) == SnapshotResult::Busy, "reader fence protects both snapshots");
    Check(!pool.Resource(tickets[0]) && pool.Discard(tickets[1]) == SnapshotResult::Stale, "submitted ticket consumed exactly once");
    Check(pool.CloseIfIdle() == SnapshotResult::Busy, "GPU reader prevents teardown");
    Hr(gate->Signal(1), "release test gate");
    Check(pool.ReaderFence()->GetCompletedValue() < pool.LastReaderSignal(), "backend still waits for independent engine");
    Check(pool.Capture(source.Get(), {100, 3}, extra) == SnapshotResult::Busy, "independent reader retains inputs");
    Hr(external_gate->Signal(1), "release independent engine test gate");
    Wait(pool.ReaderFence(), pool.LastReaderSignal());
    a.Verify(first); b.Verify(second);
    Check(pool.Collect() == SnapshotResult::Ok, "retire read leases");
    Check(pool.Capture(source.Get(), {100, 3}, extra) == SnapshotResult::Ok, "reuse only after reader completion");
    Check(extra.serial > tickets[1].serial && !pool.Resource(tickets[0]), "recycled slot cannot resurrect old ticket");

    // A different D3D11 device on the SAME WARP adapter is still invalid.
    ComPtr<ID3D11Device> foreign;
    ComPtr<ID3D11Texture2D> foreign_source;
    Hr(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &foreign, nullptr, nullptr), "foreign device");
    Hr(foreign->CreateTexture2D(&desc, nullptr, &foreign_source), "foreign source");
    SnapshotTicket invalid;
    Check(pool.Capture(foreign_source.Get(), {200, 4}, invalid) == SnapshotResult::Invalid, "foreign device rejected");
    RoundTrip(pool, extra, d11.Get(), context.Get(), d12.Get(), queue.Get(), format, third);
    Check(pool.Discard(extra) == SnapshotResult::Stale, "submitted input remains consumed after round-trip");
    Check(pool.Capture(source.Get(), {100, 4}, extra) == SnapshotResult::Ok, "capture a lease to discard");
    Check(pool.PinExternalReads(&extra, 1, external_done.Get(), 1) == SnapshotResult::Invalid, "completed external value is not a new read");
    Check(pool.PinExternalReads(&extra, 1, external_done.Get(), 2) == SnapshotResult::Ok, "pin discarded external read");
    Hr(external_queue->Wait(pool.ProducerFence(), extra.producer_fence), "external discard producer ordering");
    Hr(external_queue->Wait(external_gate.Get(), 2), "hold discarded external read");
    Hr(external_queue->Signal(external_done.Get(), 2), "discarded external completion");
    Check(pool.Discard(extra) == SnapshotResult::Ok, "discard unused captured lease");
    Check(pool.Discard(extra) == SnapshotResult::Stale, "discard is single use");
    // Drain a discarded producer using a test-only fence on the host context.
    ComPtr<ID3D11Device5> d11v5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> drain;
    Hr(d11.As(&d11v5), "device5"); Hr(context.As(&context4), "context4");
    Hr(d11v5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&drain)), "test drain fence");
    Hr(context4->Signal(drain.Get(), 1), "test drain signal");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "producer drain event");
    Hr(drain->SetEventOnCompletion(1, event), "producer drain registration");
    context->Flush();
    const DWORD wait = WaitForSingleObject(event, 15000); CloseHandle(event);
    Check(wait == WAIT_OBJECT_0, "producer drained within test timeout");
    Check(pool.CloseIfIdle() == SnapshotResult::Busy, "discard still retains unfinished external reader");
    Hr(external_gate->Signal(2), "release discarded external reader");
    Wait(external_done.Get(), 2);
    Check(pool.CloseIfIdle() == SnapshotResult::Ok, "safe nonblocking teardown after retirement");
    Check(pool.Initialize(context.Get(), d12.Get(), queue.Get(), 77, kWidth, kHeight, format, 2)
        == E_INVALIDARG, "epoch cannot be reused after teardown");
    Hr(pool.Initialize(context.Get(), d12.Get(), queue.Get(), 78, kWidth, kHeight, format, 2), "new epoch after drain");
    Check(!pool.Resource(extra), "previous session ticket rejected");
    Check(pool.CloseIfIdle() == SnapshotResult::Ok, "empty pool teardown");
    std::cout << "PASS shared snapshots format=" << static_cast<uint32_t>(format)
        << ": same-source content isolation, pair fences, bounded reuse, stale tickets, foreign device\n";
}
} // namespace
int main() {
    try { Run(DXGI_FORMAT_R8G8B8A8_UNORM); Run(DXGI_FORMAT_B8G8R8A8_UNORM); return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
