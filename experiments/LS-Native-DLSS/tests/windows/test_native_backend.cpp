#include "../../backend/native_backend.h"
#include <d3dcompiler.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <iostream>
#include <vector>
#include <stdexcept>

using namespace ls_native;
using Microsoft::WRL::ComPtr;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Hr(HRESULT value, const char* message) { Require(SUCCEEDED(value), message); }
struct Fixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    std::array<ComPtr<ID3D11Texture2D>, 4> images;
    std::array<ComPtr<ID3D11ShaderResourceView>, 4> views;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 4> outputs;
    ComPtr<ID3D11ComputeShader> native_shader;
    ComPtr<ID3D11Buffer> constants;
    TextureObservation extent;
    uint64_t luid = 0, tick = 0, generation = 0, epoch = 1, source_time = 1;
    Fixture() {
        Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context), "WARP device");
        Hr(device.As(&device5), "device5"); Hr(context.As(&context4), "context4");
        Hr(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "test fence");
        ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC ad{};
        Hr(device.As(&dxgi), "dxgi device"); Hr(dxgi->GetAdapter(&adapter), "adapter"); Hr(adapter->GetDesc(&ad), "adapter desc");
        luid = uint64_t(static_cast<uint32_t>(ad.AdapterLuid.HighPart)) << 32 | ad.AdapterLuid.LowPart;
        D3D11_TEXTURE2D_DESC td{}; td.Width = 19; td.Height = 7; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        for (size_t i = 0; i != images.size(); ++i) {
            Hr(device->CreateTexture2D(&td, nullptr, &images[i]), "native texture");
            Hr(device->CreateShaderResourceView(images[i].Get(), nullptr, &views[i]), "native SRV");
            Hr(device->CreateUnorderedAccessView(images[i].Get(), nullptr, &outputs[i]), "native UAV");
        }
        extent.width = td.Width; extent.height = td.Height; extent.format = extent.view_format = td.Format;
        extent.samples = extent.array_size = 1; extent.bind_flags = td.BindFlags;
        const char source[] = "RWTexture2D<float4> O:register(u0);[numthreads(16,16,1)]void main(uint3 p:SV_DispatchThreadID){O[p.xy]=float4(1,0,1,1);}";
        ComPtr<ID3DBlob> code, error;
        Hr(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, &error), "native shader compile");
        Hr(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &native_shader), "native shader");
        D3D11_BUFFER_DESC cb{}; cb.ByteWidth = 48; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Hr(device->CreateBuffer(&cb, nullptr, &constants), "native CB");
    }
    void Drain() {
        // Only tests block/flush. Production backend contains neither operation.
        Hr(context4->Signal(fence.Get(), ++tick), "test signal"); context->Flush();
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); Require(event != nullptr, "test event");
        const HRESULT hr = fence->SetEventOnCompletion(tick, event);
        const DWORD wait = SUCCEEDED(hr) ? WaitForSingleObject(event, 15000) : WAIT_FAILED;
        CloseHandle(event); Require(wait == WAIT_OBJECT_0, "GPU graph completion / deadlock");
    }
    SourceWriteObservation Source(size_t index, uint8_t color) {
        std::vector<uint32_t> pixels(size_t(extent.width) * extent.height);
        for (size_t p = 0; p != pixels.size(); ++p) pixels[p] = uint32_t(color + p % 8) | (uint32_t(color) << 8) | 0xff000000u;
        context->UpdateSubresource(images[index].Get(), 0, nullptr, pixels.data(), extent.width * 4, 0);
        SourceWriteObservation w; w.epoch = epoch; w.generation = ++generation; w.sequence = generation * 2;
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        source_time += static_cast<uint64_t>(frequency.QuadPart) / 68; w.qpc = source_time;
        w.device = w.context = 1; w.adapter_luid = luid; w.thread = GetCurrentThreadId();
        w.texture = extent; w.texture.object = index + 1; return w;
    }
    DispatchObservation Slot(const SourceWriteObservation& p, const SourceWriteObservation& c) {
        DispatchObservation r; r.previous_write = p; r.current_write = c; r.pair_matches_observed_updates = true;
        r.constants_known = true; r.phase_bits = 0x3f000000; r.output = extent; r.output.object = 4;
        r.device = r.context = 1; r.adapter_luid = luid; r.thread = GetCurrentThreadId();
        return r;
    }
    void Native() {
        ID3D11ShaderResourceView* v[]{views[0].Get(), views[1].Get()}; context->CSSetShaderResources(0, 2, v);
        auto* output = outputs[3].Get(); context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
        context->CSSetShader(native_shader.Get(), nullptr, 0); auto* cb = constants.Get(); context->CSSetConstantBuffers(0, 1, &cb);
        context->Dispatch(2, 1, 1);
    }
    void Unbind() {
        ID3D11ShaderResourceView* v[2]{}; ID3D11UnorderedAccessView* u = nullptr;
        context->CSSetShaderResources(0, 2, v); context->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
    }
    void Pixels(uint8_t expected) {
        Unbind(); Drain(); D3D11_TEXTURE2D_DESC td{}; images[3]->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.BindFlags = 0;
        ComPtr<ID3D11Texture2D> read; Hr(device->CreateTexture2D(&td, nullptr, &read), "readback");
        context->CopyResource(read.Get(), images[3].Get()); Drain();
        D3D11_MAPPED_SUBRESOURCE m{}; Hr(context->Map(read.Get(), 0, D3D11_MAP_READ, 0, &m), "pixel map");
        bool valid = true;
        for (UINT y = 0; y != td.Height; ++y) for (UINT x = 0; x != td.Width; ++x) {
            const auto* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
            if (expected == 255) valid &= p[0] == 255 && p[1] == 0 && p[2] == 255 && p[3] == 255;
            else valid &= p[0] == expected + (y * td.Width + x) % 8 && p[1] == expected && p[2] == 0 && p[3] == 255;
        }
        context->Unmap(read.Get(), 0); Require(valid, "conditional replacement pixels");
    }
    void Bindings() {
        ComPtr<ID3D11ComputeShader> shader; context->CSGetShader(&shader, nullptr, nullptr);
        Require(shader.Get() == native_shader.Get(), "shader restored");
        ID3D11ShaderResourceView* raw[2]{}; context->CSGetShaderResources(0, 2, raw);
        const bool same = raw[0] == views[0].Get() && raw[1] == views[1].Get();
        for (auto* v : raw) if (v) v->Release();
        Require(same, "SRVs restored");
        ComPtr<ID3D11Buffer> cb; context->CSGetConstantBuffers(0, 1, &cb); Require(cb.Get() == constants.Get(), "CB restored");
        ComPtr<ID3D11UnorderedAccessView> uav; context->CSGetUnorderedAccessViews(0, 1, &uav);
        Require(uav.Get() == outputs[3].Get(), "UAV unchanged");
    }
};
void Test(bool disabled) {
    Fixture f; NativeBackend backend; BackendOptions o; o.enabled = o.synthetic_test = true; o.test_disable = disabled;
    const HRESULT initialized = backend.Initialize(f.context.Get(), f.extent, o, L".");
    if (FAILED(initialized)) std::cerr << backend.FailureStep() << " 0x" << std::hex << backend.ErrorCode() << std::dec << '\n';
    Hr(initialized, "backend initialize");
    const auto a = f.Source(0, 20); Require(!backend.Source(f.images[0].Get(), a), "first source fallback");
    const auto b = f.Source(1, 40); Require(backend.Source(f.images[1].Get(), b), "warmup submission");
    f.Native(); Require(!backend.Composite(f.Slot(a, b), f.outputs[3].Get()), "warmup never presented"); f.Pixels(255);
    const auto c = f.Source(0, 80); Require(backend.Source(f.images[0].Get(), c), "second submission");
    f.Native(); auto slot = f.Slot(b, c);
    auto bad = slot; bad.phase_bits = 0x3e800000; Require(!backend.Composite(bad, f.outputs[3].Get()), "wrong phase");
    bad = slot; std::swap(bad.previous_write, bad.current_write); Require(!backend.Composite(bad, f.outputs[3].Get()), "reversed pair");
    bad = slot; ++bad.current_write.generation; Require(!backend.Composite(bad, f.outputs[3].Get()), "stale source");
    Require(!backend.Composite(slot, f.outputs[2].Get()), "wrong currently bound output");
    Require(backend.Composite(slot, f.outputs[3].Get()), "GPU ordered composite queued"); f.Bindings();
    Require(!backend.Composite(slot, f.outputs[3].Get()), "same slot never consumed twice"); f.Pixels(disabled ? 255 : 60);
    auto counts = backend.PollCounters(); Require(counts.submitted == 2 && counts.composite_queued == 1 && !counts.failed, "counts");
    Require(disabled ? counts.gpu_disabled > 0 : counts.gpu_enabled > 0, "actual GPU disable flag observed");
    ++f.epoch; f.generation = 0;
    auto d = f.Source(1, 100); Require(!backend.Source(f.images[1].Get(), d), "epoch reset source");
    auto e = f.Source(0, 120); Require(backend.Source(f.images[0].Get(), e), "reset evaluation");
    f.Native(); Require(!backend.Composite(f.Slot(d, e), f.outputs[3].Get()), "reset warmup fallback"); f.Pixels(255);
    auto g = f.Source(1, 140); Require(backend.Source(f.images[1].Get(), g), "resume source");
    f.Native(); Require(backend.Composite(f.Slot(e, g), f.outputs[3].Get()), "resume composite"); f.Pixels(disabled ? 255 : 130);
    std::cout << "GPU transport, exact-pair gate, " << (disabled ? "disable flag fallback" : "pixel replacement") << ", state restoration and reset passed\n";
}
void TestRetirement(bool ordered) {
    Fixture f; NativeBackend backend; BackendOptions options;
    options.enabled = options.synthetic_test = true; options.slots = 2; options.gpu_ordered = ordered;
    Hr(backend.Initialize(f.context.Get(), f.extent, options, L"."), "retirement backend");
    auto a = f.Source(0, 20); backend.Source(f.images[0].Get(), a);
    auto b = f.Source(1, 40); Require(backend.Source(f.images[1].Get(), b), "retirement warmup");
    auto completed = [&](uint64_t samples) {
        f.Drain(); const auto deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < deadline) {
            const auto counts = backend.PollCounters();
            if (counts.gpu_enabled + counts.gpu_disabled >= samples) return;
            Sleep(1); // TEST ONLY
        }
        throw std::runtime_error("backend did not complete");
    };
    completed(1);
    auto c = f.Source(0, 80); Require(backend.Source(f.images[0].Get(), c), "retirement second pair"); completed(2);
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<ID3D12Device> gate_device;
    Hr(f.device.As(&dxgi), "gate dxgi"); Hr(dxgi->GetAdapter(&adapter), "gate adapter");
    Hr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gate_device)), "gate device");
    ComPtr<ID3D12Fence> gate12; ComPtr<ID3D11Fence> gate11;
    Hr(gate_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate12)), "gate fence");
    HANDLE handle = nullptr; Hr(gate_device->CreateSharedHandle(gate12.Get(), nullptr, GENERIC_ALL, nullptr, &handle), "gate handle");
    const HRESULT opened = f.device5->OpenSharedFence(handle, IID_PPV_ARGS(&gate11)); CloseHandle(handle); Hr(opened, "gate import");
    struct ReleaseGate { ID3D12Fence* fence; ~ReleaseGate() { fence->Signal(1); } } release{gate12.Get()};
    Hr(f.context4->Wait(gate11.Get(), 1), "hold native retirement");
    f.Native(); Require(backend.Composite(f.Slot(b, c), f.outputs[3].Get()), "ready result queued behind held native fence");
    f.Unbind();
    auto d = f.Source(1, 100); Require(backend.Source(f.images[1].Get(), d), "second ring slot");
    if (!ordered) {
        f.Native(); Require(!backend.Composite(f.Slot(c, d), f.outputs[3].Get()), "ready-only rejects unfinished GPU result");
        f.Unbind();
    }
    auto e = f.Source(0, 120);
    Require(!backend.Source(f.images[0].Get(), e), "completed output is still leased until native retirement");
    Require(backend.PollCounters().busy > 0, "bounded ring busy fallback");
    Hr(gate12->Signal(1), "release native retirement"); f.Pixels(ordered ? 60 : 255);
    auto next = f.Source(1, 140); Require(backend.Source(f.images[1].Get(), next), "retired ring reusable");
    f.Native(); Require(!backend.Composite(f.Slot(e, next), f.outputs[3].Get()), "dropped evaluation resets history"); f.Pixels(255);
    std::cout << "Held native retirement, bounded ring reuse and " << (ordered ? "GPU ordering" : "ready-only fallback") << " passed\n";
}
}
int main() { try { Test(false); Test(true); TestRetirement(true); TestRetirement(false); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
