#include "native_backend.h"
#include "native_shaders.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#include <nvOpticalFlowD3D12.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>

namespace ls_native {
using Microsoft::WRL::ComPtr;
namespace {
template<class F> NVSDK_NGX_Result Ngx(const F& fn) {
#ifdef _MSC_VER
    __try { return fn(); } __except(EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_FAIL_PlatformError; }
#else
    return fn();
#endif
}
template<class F> NV_OF_STATUS Of(const F& fn) {
#ifdef _MSC_VER
    __try { return fn(); } __except(EXCEPTION_EXECUTE_HANDLER) { return NV_OF_ERR_GENERIC; }
#else
    return fn();
#endif
}
bool Same(IUnknown* a, IUnknown* b) {
    ComPtr<IUnknown> x, y;
    return a && b && SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x.Get() == y.Get();
}
uint64_t Luid(LUID v) { return uint64_t(static_cast<uint32_t>(v.HighPart)) << 32 | v.LowPart; }
D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to}; return b;
}
HRESULT Compile(const char* text, const char* entry, const char* target, ID3DBlob** output) {
    ComPtr<ID3DBlob> error;
    return D3DCompile(text, std::strlen(text), "NativeDLSS own shader", nullptr, nullptr, entry, target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, output, &error);
}
bool FormatSupported(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R16G16B16A16_FLOAT;
}
}

struct NativeBackend::State {
    struct Job {
        ComPtr<ID3D11Texture2D> inputs11[2], output11, flag11;
        ComPtr<ID3D12Resource> inputs12[2], output12, flag12, analysis[2], coarse, motion, depth, disabled, readback;
        ComPtr<ID3D11ShaderResourceView> output_view, flag_view;
        ComPtr<ID3D12DescriptorHeap> heap, cpu, targets;
        ComPtr<ID3D12CommandAllocator> prepare_alloc, generate_alloc;
        ComPtr<ID3D12GraphicsCommandList> prepare, generate;
        NvOFGPUBufferHandle registered[3]{};
        SourceWriteObservation previous, current;
        uint64_t ready = 0, copied = 0, polled = 0;
        bool consumed = false, warmup = true;
    };
    std::mutex mutex;
    BackendOptions options;
    TextureObservation extent;
    ComPtr<ID3D11Device5> native_device;
    ComPtr<ID3D11DeviceContext4> native_context;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D11Fence> producer11, ready11, copied11;
    ComPtr<ID3D12Fence> producer12, ready12, converted, flow_done, registered_done;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> input_pso, dense_pso, flag_pso;
#ifdef LS_NATIVE_TEST
    ComPtr<ID3D12PipelineState> synthetic_pso;
#endif
    ComPtr<ID3D11ComputeShader> composite_shader;
    std::array<Job, 4> jobs;
    SourceWriteObservation last_write;
    ComPtr<ID3D11Texture2D> last_source;
    NV_OF_D3D12_API_FUNCTION_LIST of{};
    HMODULE of_module = nullptr;
    NvOFHandle of_session = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    uint32_t analysis_width = 0, analysis_height = 0, grid = 4, slots = 3;
    DXGI_FORMAT analysis_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    uint64_t producer_value = 0, ready_value = 0, converted_value = 0, flow_value = 0, register_value = 0;
    uint64_t copy_value = 0, last_evaluated_epoch = 0, last_evaluated_generation = 0, frame_id = 0;
    BackendCounters counters;
    const char* step = "not initialized";
    uint32_t code = 0;
    bool failed = false, initialized = false;

    bool Fail(const char* where, uint32_t value = 0) {
        step = where; code = value; failed = true; ++counters.failed; return false;
    }
    HRESULT Check(HRESULT hr, const char* where) { if (FAILED(hr)) Fail(where, static_cast<uint32_t>(hr)); return hr; }
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(Job& j, UINT index, bool visible = false) const {
        auto h = (visible ? j.heap : j.cpu)->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(index) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(Job& j, UINT index) const {
        auto h = j.heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += UINT64(index) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); return h;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(Job& j, UINT index) const {
        auto h = j.targets->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(index) * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV); return h;
    }
    HRESULT PrivateTexture(uint32_t w, uint32_t h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, ID3D12Resource** out) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h;
        d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Format = format; d.Flags = flags;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(out));
    }
    HRESULT Buffer(uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ID3D12Resource** out) {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
        d.DepthOrArraySize = d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
        D3D12_HEAP_PROPERTIES h{}; h.Type = type;
        return device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(out));
    }
    HRESULT SharedTexture(uint32_t w, uint32_t h, DXGI_FORMAT format, UINT bind,
                          ID3D11Texture2D** a, ID3D12Resource** b) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.Format = format;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1; d.BindFlags = bind;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        HRESULT hr = native_device->CreateTexture2D(&d, nullptr, a); if (FAILED(hr)) return hr;
        ComPtr<IDXGIResource1> r; if (FAILED(hr = (*a)->QueryInterface(IID_PPV_ARGS(&r)))) return hr;
        HANDLE handle = nullptr;
        if (FAILED(hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) return hr;
        hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(b)); CloseHandle(handle); return hr;
    }
    HRESULT Fences() {
        HRESULT hr = native_device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&producer11));
        if (FAILED(hr)) return hr;
        HANDLE h = nullptr;
        if (FAILED(hr = producer11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h))) return hr;
        hr = device->OpenSharedHandle(h, IID_PPV_ARGS(&producer12)); CloseHandle(h); if (FAILED(hr)) return hr;
        if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready12)))) return hr;
        if (FAILED(hr = device->CreateSharedHandle(ready12.Get(), nullptr, GENERIC_ALL, nullptr, &h))) return hr;
        hr = native_device->OpenSharedFence(h, IID_PPV_ARGS(&ready11)); CloseHandle(h); if (FAILED(hr)) return hr;
        if (FAILED(hr = native_device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&copied11)))) return hr;
        if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&converted)))) return hr;
        if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&flow_done)))) return hr;
        return device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&registered_done));
    }
    HRESULT Compute(const char* shader, ID3D12PipelineState** result) {
        ComPtr<ID3DBlob> code_blob; HRESULT hr = Compile(shader, "main", "cs_5_0", &code_blob); if (FAILED(hr)) return hr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{}; p.pRootSignature = root.Get();
        p.CS = {code_blob->GetBufferPointer(), code_blob->GetBufferSize()}; return device->CreateComputePipelineState(&p, IID_PPV_ARGS(result));
    }
    HRESULT Shaders() {
        D3D12_DESCRIPTOR_RANGE ranges[3]{}; D3D12_ROOT_PARAMETER parameters[4]{};
        for (UINT i = 0; i != 3; ++i) {
            ranges[i].RangeType = i == 2 ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors = 1; ranges[i].BaseShaderRegister = i == 1 ? 1 : 0;
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable = {1, &ranges[i]}; parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; parameters[3].Constants = {0, 0, 8};
        D3D12_STATIC_SAMPLER_DESC sampler{}; sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        D3D12_ROOT_SIGNATURE_DESC rd{}; rd.NumParameters = 4; rd.pParameters = parameters;
        rd.NumStaticSamplers = 1; rd.pStaticSamplers = &sampler; rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature, error; HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
        if (FAILED(hr)) return hr;
        if (FAILED(hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&root)))) return hr;
        if (FAILED(hr = Compute(shaders::densify, &dense_pso)) || FAILED(hr = Compute(shaders::flag, &flag_pso))) return hr;
        ComPtr<ID3DBlob> vs, ps, cs;
        if (FAILED(hr = Compile(shaders::input, "vs", "vs_5_0", &vs)) || FAILED(hr = Compile(shaders::input, "ps", "ps_5_0", &ps)) ||
            FAILED(hr = Compile(shaders::composite, "main", "cs_5_0", &cs))) return hr;
        if (FAILED(hr = native_device->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &composite_shader))) return hr;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p{}; p.pRootSignature = root.Get();
        p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()}; p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        p.BlendState.RenderTarget[0].SrcBlend = p.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        p.BlendState.RenderTarget[0].DestBlend = p.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
        p.BlendState.RenderTarget[0].BlendOp = p.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        p.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
        p.SampleMask = UINT_MAX; p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE; p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        p.DepthStencilState.FrontFace.StencilFunc = p.DepthStencilState.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        p.DepthStencilState.FrontFace.StencilFailOp = p.DepthStencilState.FrontFace.StencilDepthFailOp = p.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        p.DepthStencilState.BackFace = p.DepthStencilState.FrontFace;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; p.NumRenderTargets = 1;
        p.RTVFormats[0] = analysis_format; p.SampleDesc.Count = 1;
        if (FAILED(hr = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&input_pso)))) return hr;
#ifdef LS_NATIVE_TEST
        if (options.synthetic_test && FAILED(hr = Compute(shaders::synthetic, &synthetic_pso))) return hr;
#endif
        return S_OK;
    }
    bool OpticalFlow() {
        of_module = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); if (!of_module) return Fail("NVOF driver library", GetLastError());
        const auto address = GetProcAddress(of_module, "NvOFAPICreateInstanceD3D12");
        decltype(&NvOFAPICreateInstanceD3D12) entry = nullptr; std::memcpy(&entry, &address, sizeof(entry));
        if (!entry || Of([&]{return entry(NV_OF_API_VERSION, &of);}) != NV_OF_SUCCESS ||
            !of.nvCreateOpticalFlowD3D12 || !of.nvOFInit || !of.nvOFRegisterResourceD3D12 ||
            !of.nvOFExecuteD3D12 || !of.nvOFGetCaps || !of.nvOFGetSurfaceFormatCountD3D12 || !of.nvOFGetSurfaceFormatD3D12)
            return Fail("NVOF D3D12 entry points");
        const auto created = Of([&]{return of.nvCreateOpticalFlowD3D12(device.Get(), &of_session);});
        if (created != NV_OF_SUCCESS || !of_session) return Fail("NVOF session", static_cast<uint32_t>(created));
        auto formats = [&](NV_OF_BUFFER_USAGE usage, DXGI_FORMAT desired) {
            uint32_t n = 0;
            if (Of([&]{return of.nvOFGetSurfaceFormatCountD3D12(of_session, usage, NV_OF_MODE_OPTICALFLOW, &n);}) != NV_OF_SUCCESS || !n || n > 64) return false;
            std::vector<DXGI_FORMAT> f(n);
            return Of([&]{return of.nvOFGetSurfaceFormatD3D12(of_session, usage, NV_OF_MODE_OPTICALFLOW, f.data());}) == NV_OF_SUCCESS &&
                std::find(f.begin(), f.end(), desired) != f.end();
        };
        if (!formats(NV_OF_BUFFER_USAGE_INPUT, analysis_format)) {
            analysis_format = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (!formats(NV_OF_BUFFER_USAGE_INPUT, analysis_format)) return Fail("NVOF input formats");
        }
        if (!formats(NV_OF_BUFFER_USAGE_OUTPUT, DXGI_FORMAT_R16G16_SINT)) return Fail("NVOF output format");
        uint32_t n = 0;
        if (Of([&]{return of.nvOFGetCaps(of_session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &n);}) != NV_OF_SUCCESS || !n || n > 64) return Fail("NVOF grid count");
        std::vector<uint32_t> grids(n);
        if (Of([&]{return of.nvOFGetCaps(of_session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids.data(), &n);}) != NV_OF_SUCCESS || n > grids.size()) return Fail("NVOF grids");
        grid = options.quality >= 4 ? 2u : 4u;
        if (std::find(grids.begin(), grids.begin() + n, grid) == grids.begin() + n) grid = 4;
        if (std::find(grids.begin(), grids.begin() + n, grid) == grids.begin() + n) return Fail("NVOF supported grid");
        auto dimension = [&](NV_OF_CAPS cap, uint32_t& value) {
            uint32_t count = 0;
            if (Of([&]{return of.nvOFGetCaps(of_session, cap, nullptr, &count);}) != NV_OF_SUCCESS || count != 1) return false;
            return Of([&]{return of.nvOFGetCaps(of_session, cap, &value, &count);}) == NV_OF_SUCCESS && count == 1;
        };
        uint32_t minw = 0, minh = 0, maxw = 0, maxh = 0;
        if (!dimension(NV_OF_CAPS_WIDTH_MIN, minw) || !dimension(NV_OF_CAPS_HEIGHT_MIN, minh) ||
            !dimension(NV_OF_CAPS_WIDTH_MAX, maxw) || !dimension(NV_OF_CAPS_HEIGHT_MAX, maxh)) return Fail("NVOF dimension caps");
        analysis_width = std::max(analysis_width, minw); analysis_height = std::max(analysis_height, minh);
        if (analysis_width > maxw || analysis_height > maxh) return Fail("NVOF analysis extent");
        NV_OF_INIT_PARAMS p{}; p.width = analysis_width; p.height = analysis_height;
        p.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid); p.mode = NV_OF_MODE_OPTICALFLOW;
        p.perfLevel = options.quality == 1 ? NV_OF_PERF_LEVEL_FAST :
            (options.quality == 3 || options.quality == 5) ? NV_OF_PERF_LEVEL_SLOW : NV_OF_PERF_LEVEL_MEDIUM;
        p.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8; p.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
        const auto result = Of([&]{return of.nvOFInit(of_session, &p);});
        return result == NV_OF_SUCCESS || Fail("NVOF init", static_cast<uint32_t>(result));
    }
    void Srv(Job& j, UINT index, ID3D12Resource* resource, DXGI_FORMAT format, bool raw = false) {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{}; d.Format = format; d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (raw) { d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER; d.Buffer.NumElements = 1; d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW; }
        else { d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; d.Texture2D.MipLevels = 1; }
        device->CreateShaderResourceView(resource, &d, Cpu(j, index));
        device->CopyDescriptorsSimple(1, Cpu(j, index, true), Cpu(j, index), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    void Uav(Job& j, UINT index, ID3D12Resource* resource, DXGI_FORMAT format) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{}; d.Format = format; d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(resource, nullptr, &d, Cpu(j, index));
        device->CopyDescriptorsSimple(1, Cpu(j, index, true), Cpu(j, index), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    HRESULT JobResources(Job& j) {
        HRESULT hr;
        const auto format = static_cast<DXGI_FORMAT>(extent.format);
        for (UINT i = 0; i != 2; ++i) {
            if (FAILED(hr = SharedTexture(extent.width, extent.height, format, D3D11_BIND_SHADER_RESOURCE, &j.inputs11[i], &j.inputs12[i]))) return hr;
            if (FAILED(hr = PrivateTexture(analysis_width, analysis_height, analysis_format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, &j.analysis[i]))) return hr;
        }
        if (FAILED(hr = SharedTexture(extent.width, extent.height, format, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &j.output11, &j.output12)) ||
            FAILED(hr = SharedTexture(1, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &j.flag11, &j.flag12)) ||
            FAILED(hr = native_device->CreateShaderResourceView(j.output11.Get(), nullptr, &j.output_view)) ||
            FAILED(hr = native_device->CreateShaderResourceView(j.flag11.Get(), nullptr, &j.flag_view))) return hr;
        if (FAILED(hr = PrivateTexture((analysis_width + grid - 1) / grid, (analysis_height + grid - 1) / grid, DXGI_FORMAT_R16G16_SINT, D3D12_RESOURCE_FLAG_NONE, &j.coarse)) ||
            FAILED(hr = PrivateTexture(extent.width, extent.height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &j.motion)) ||
            FAILED(hr = PrivateTexture(extent.width, extent.height, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, &j.depth)) ||
            FAILED(hr = Buffer(4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &j.disabled)) ||
            FAILED(hr = Buffer(4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &j.readback))) return hr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 9; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&j.heap)))) return hr;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; if (FAILED(hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&j.cpu)))) return hr;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 2;
        if (FAILED(hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&j.targets)))) return hr;
        Srv(j, 0, j.inputs12[0].Get(), format); Srv(j, 1, j.inputs12[1].Get(), format); Srv(j, 2, j.coarse.Get(), DXGI_FORMAT_R16G16_SINT);
        Uav(j, 3, j.motion.Get(), DXGI_FORMAT_R16G16_FLOAT); Srv(j, 4, j.disabled.Get(), DXGI_FORMAT_R32_TYPELESS, true);
        Uav(j, 5, j.flag12.Get(), DXGI_FORMAT_R32_UINT); Uav(j, 6, j.output12.Get(), format); Uav(j, 7, j.depth.Get(), DXGI_FORMAT_R32_FLOAT);
        D3D12_UNORDERED_ACCESS_VIEW_DESC raw{}; raw.Format = DXGI_FORMAT_R32_TYPELESS;
        raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; raw.Buffer.NumElements = 1; raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(j.disabled.Get(), nullptr, &raw, Cpu(j, 8));
        device->CopyDescriptorsSimple(1, Cpu(j, 8, true), Cpu(j, 8), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT i = 0; i != 2; ++i) device->CreateRenderTargetView(j.analysis[i].Get(), nullptr, Rtv(j, i));
        if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&j.prepare_alloc))) ||
            FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&j.generate_alloc))) ||
            FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, j.prepare_alloc.Get(), nullptr, IID_PPV_ARGS(&j.prepare))) ||
            FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, j.generate_alloc.Get(), nullptr, IID_PPV_ARGS(&j.generate)))) return hr;
        if (FAILED(hr = j.prepare->Close()) || FAILED(hr = j.generate->Close())) return hr;
        if (options.optical_flow) {
            for (UINT i = 0; i != 3; ++i) {
                NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 r{}; r.resource = i == 2 ? j.coarse.Get() : j.analysis[i].Get();
                r.hOFGpuBuffer = &j.registered[i]; r.inputFencePoint = {converted.Get(), 0};
                r.outputFencePoint = {registered_done.Get(), ++register_value};
                const auto result = Of([&]{return of.nvOFRegisterResourceD3D12(of_session, &r);});
                if (result != NV_OF_SUCCESS || !j.registered[i]) { Fail("NVOF register", static_cast<uint32_t>(result)); return E_FAIL; }
            }
        }
        return S_OK;
    }
    bool NgxInit(const std::filesystem::path& folder) {
        const std::wstring runtime = (folder / L"native-runtime").wstring();
        const std::wstring old = (folder / L"addons" / L"LS_DLSSFG" / L"runtime").wstring();
        const std::wstring app = folder.wstring(), cache = (folder / L"native-dlss-cache").wstring();
        const wchar_t* paths[]{runtime.c_str(), old.c_str(), app.c_str()};
        NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo.Path = paths; info.PathListInfo.Length = 3;
        auto result = Ngx([&]{return NVSDK_NGX_D3D12_Init_with_ProjectID("eae67294-65c6-4bb7-a34a-51df0b8c95de",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM, "LS-Native-DLSS-0.1", cache.c_str(), device.Get(), &info, NVSDK_NGX_Version_API);});
        if (!NVSDK_NGX_SUCCEED(result)) return Fail("NGX init", static_cast<uint32_t>(result));
        result = Ngx([&]{return NVSDK_NGX_D3D12_GetCapabilityParameters(&params);});
        if (!NVSDK_NGX_SUCCEED(result) || !params) return Fail("NGX capability parameters", static_cast<uint32_t>(result));
        int available = 0;
        result = Ngx([&]{return NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_FrameGeneration_Available, &available);});
        if (!NVSDK_NGX_SUCCEED(result) || !available) return Fail("FG unavailable: compatible runtime required", static_cast<uint32_t>(result));
        const unsigned unused = NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_UI |
            NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField | NVSDK_NGX_DLSSG_ResourceFlags_OutputReal;
        result = Ngx([&]{NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, unused); return NVSDK_NGX_Result_Success;});
        if (!NVSDK_NGX_SUCCEED(result)) return Fail("NGX resource flags", static_cast<uint32_t>(result));
        auto& j = jobs[0];
        if (FAILED(j.generate_alloc->Reset()) || FAILED(j.generate->Reset(j.generate_alloc.Get(), nullptr))) return Fail("NGX init command reset");
        NVSDK_NGX_DLSSG_Create_Params p{}; p.Width = p.RenderWidth = extent.width; p.Height = p.RenderHeight = extent.height; p.NativeBackbufferFormat = extent.format;
        result = Ngx([&]{return NGX_D3D12_CREATE_DLSSG(j.generate.Get(), 1, 1, &feature, params, &p);});
        if (!NVSDK_NGX_SUCCEED(result) || !feature || FAILED(j.generate->Close())) return Fail("NGX create feature", static_cast<uint32_t>(result));
        ID3D12CommandList* lists[]{j.generate.Get()}; queue->ExecuteCommandLists(1, lists);
        j.ready = ++ready_value;
        if (FAILED(queue->Signal(ready12.Get(), j.ready))) return Fail("NGX init GPU signal");
        j.polled = j.ready; // Initialization does not contain a disable flag.
        return true;
    }
    void Root(Job& j, ID3D12GraphicsCommandList* cmd, bool graphics, UINT srv0, UINT srv1, UINT uav) {
        ID3D12DescriptorHeap* heaps[]{j.heap.Get()}; cmd->SetDescriptorHeaps(1, heaps);
        if (graphics) {
            cmd->SetGraphicsRootSignature(root.Get()); cmd->SetGraphicsRootDescriptorTable(0, Gpu(j, srv0));
            cmd->SetGraphicsRootDescriptorTable(1, Gpu(j, srv1)); cmd->SetGraphicsRootDescriptorTable(2, Gpu(j, uav));
        } else {
            cmd->SetComputeRootSignature(root.Get()); cmd->SetComputeRootDescriptorTable(0, Gpu(j, srv0));
            cmd->SetComputeRootDescriptorTable(1, Gpu(j, srv1)); cmd->SetComputeRootDescriptorTable(2, Gpu(j, uav));
        }
    }
    bool Generate(Job& j, bool reset) {
        if (FAILED(j.prepare_alloc->Reset()) || FAILED(j.prepare->Reset(j.prepare_alloc.Get(), input_pso.Get())) ||
            FAILED(j.generate_alloc->Reset()) || FAILED(j.generate->Reset(j.generate_alloc.Get(), nullptr))) return Fail("job allocator reset");
        if (options.optical_flow) {
            const D3D12_VIEWPORT viewport{0, 0, float(analysis_width), float(analysis_height), 0, 1};
            const D3D12_RECT rect{0, 0, LONG(analysis_width), LONG(analysis_height)};
            j.prepare->RSSetViewports(1, &viewport); j.prepare->RSSetScissorRects(1, &rect);
            j.prepare->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const uint32_t constants[8]{analysis_width, analysis_height, 0, 0, 0, 0, 0, 0};
            for (UINT i = 0; i != 2; ++i) {
                D3D12_RESOURCE_BARRIER begin[]{Barrier(j.inputs12[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                    Barrier(j.analysis[i].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
                j.prepare->ResourceBarrier(2, begin); Root(j, j.prepare.Get(), true, i, i, 3);
                j.prepare->SetGraphicsRoot32BitConstants(3, 8, constants, 0);
                const auto target = Rtv(j, i); j.prepare->OMSetRenderTargets(1, &target, FALSE, nullptr);
                j.prepare->DrawInstanced(3, 1, 0, 0);
                for (auto& b : begin) std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
                j.prepare->ResourceBarrier(2, begin);
            }
        }
        if (FAILED(j.prepare->Close())) return Fail("prepare command close");
        if (FAILED(queue->Wait(producer12.Get(), producer_value))) return Fail("producer GPU dependency");
        if (options.optical_flow && FAILED(queue->Wait(registered_done.Get(), register_value))) return Fail("NVOF registration dependency");
        ID3D12CommandList* prepared[]{j.prepare.Get()}; queue->ExecuteCommandLists(1, prepared);
        if (FAILED(queue->Signal(converted.Get(), ++converted_value))) return Fail("conversion GPU signal");
        if (options.optical_flow) {
            NV_OF_FENCE_POINT input{converted.Get(), converted_value}, output{flow_done.Get(), ++flow_value};
            NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{}; in.inputFrame = j.registered[1]; in.referenceFrame = j.registered[0];
            in.disableTemporalHints = reset ? NV_OF_TRUE : NV_OF_FALSE; in.numFencePoints = 1; in.fencePoint = &input;
            NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{}; out.outputBuffer = j.registered[2]; out.fencePoint = &output;
            const auto result = Of([&]{return of.nvOFExecuteD3D12(of_session, &in, &out);});
            if (result != NV_OF_SUCCESS) return Fail("NVOF execute", static_cast<uint32_t>(result));
            if (FAILED(queue->Wait(flow_done.Get(), flow_value))) return Fail("optical-flow GPU dependency");
        }
        auto* cmd = j.generate.Get();
        D3D12_RESOURCE_BARRIER resources[]{
            Barrier(j.inputs12[0].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(j.inputs12[1].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(j.output12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Barrier(j.motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Barrier(j.depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        cmd->ResourceBarrier(5, resources);
        Root(j, cmd, false, 2, 2, 3);
        const float zeros[4]{};
        cmd->ClearUnorderedAccessViewFloat(Gpu(j, 7), Cpu(j, 7), j.depth.Get(), zeros, 0, nullptr);
        if (options.optical_flow) {
            auto coarse_barrier = Barrier(j.coarse.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cmd->ResourceBarrier(1, &coarse_barrier); cmd->SetPipelineState(dense_pso.Get());
            uint32_t constants[8]{extent.width, extent.height, grid, 0, 0, 0, 0, 0};
            const float scale[2]{float(extent.width) / float(analysis_width), float(extent.height) / float(analysis_height)};
            std::memcpy(constants + 4, scale, sizeof(scale)); cmd->SetComputeRoot32BitConstants(3, 8, constants, 0);
            cmd->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            std::swap(coarse_barrier.Transition.StateBefore, coarse_barrier.Transition.StateAfter); cmd->ResourceBarrier(1, &coarse_barrier);
        } else cmd->ClearUnorderedAccessViewFloat(Gpu(j, 3), Cpu(j, 3), j.motion.Get(), zeros, 0, nullptr);
        D3D12_RESOURCE_BARRIER guidance[]{
            Barrier(j.motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(j.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        cmd->ResourceBarrier(2, guidance);
        bool synthetic = false;
#ifdef LS_NATIVE_TEST
        synthetic = options.synthetic_test;
        if (synthetic) {
            Root(j, cmd, false, 0, 1, 6); cmd->SetPipelineState(synthetic_pso.Get());
            cmd->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
            const UINT reject[4]{options.test_disable ? 1u : 0u, 0, 0, 0};
            cmd->ClearUnorderedAccessViewUint(Gpu(j, 8), Cpu(j, 8), j.disabled.Get(), reject, 0, nullptr);
        }
#endif
        if (!synthetic) {
            // An unwritten runtime flag must never authorize replacement.
            const UINT reject[4]{1, 1, 1, 1};
            cmd->ClearUnorderedAccessViewUint(Gpu(j, 8), Cpu(j, 8), j.disabled.Get(), reject, 0, nullptr);
            D3D12_RESOURCE_BARRIER order{}; order.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; order.UAV.pResource = j.disabled.Get();
            cmd->ResourceBarrier(1, &order);
            auto result = Ngx([&]{NVSDK_NGX_Parameter_SetULL(params, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++frame_id); return NVSDK_NGX_Result_Success;});
            if (!NVSDK_NGX_SUCCEED(result)) return Fail("NGX frame ID", static_cast<uint32_t>(result));
            NVSDK_NGX_D3D12_DLSSG_Eval_Params ep{};
            ep.pBackbuffer = j.inputs12[1].Get(); ep.pMVecs = j.motion.Get(); ep.pDepth = j.depth.Get();
            ep.pOutputInterpFrame = j.output12.Get(); ep.pOutputDisableInterpolation = j.disabled.Get();
            NVSDK_NGX_DLSSG_Opt_Eval_Params op{}; op.multiFrameCount = op.multiFrameIndex = 1; op.reset = reset;
            op.cameraMotionIncluded = op.motionVectorsDilated = options.optical_flow;
            op.motionVectorsInvalidValue = std::numeric_limits<float>::max();
            for (UINT i = 0; i != 4; ++i) op.cameraViewToClip[i][i] = op.clipToCameraView[i][i] =
                op.clipToLensClip[i][i] = op.clipToPrevClip[i][i] = op.prevClipToClip[i][i] = 1;
            // SDK specifies normalization into [-1,1], not raw pixel units.
            op.mvecScale[0] = options.motion_scale / float(extent.width); op.mvecScale[1] = options.motion_scale / float(extent.height);
            op.cameraUp[1] = op.cameraRight[0] = op.cameraFwd[2] = 1; op.cameraNear = 0.1f; op.cameraFar = 1000;
            op.cameraFOV = 1.04719755f; op.cameraAspectRatio = float(extent.width) / float(extent.height);
            op.mvecsSubrectSize = op.depthSubrectSize = op.backbufferSubrectSize = op.outputInterpSubrectSize = {extent.width, extent.height};
            result = Ngx([&]{return NGX_D3D12_EVALUATE_DLSSG(cmd, feature, params, &ep, &op);});
            if (!NVSDK_NGX_SUCCEED(result)) return Fail("NGX evaluate", static_cast<uint32_t>(result));
        }
        // Return guidance to COMMON and expose actual disable output on a shared
        // 1x1 UINT image. The native shader tests it without CPU flag readback.
        for (auto& b : guidance) { b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON; }
        cmd->ResourceBarrier(2, guidance);
        {
            D3D12_RESOURCE_BARRIER flags[]{Barrier(j.disabled.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                Barrier(j.flag12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            cmd->ResourceBarrier(2, flags); Root(j, cmd, false, 4, 4, 5); cmd->SetPipelineState(flag_pso.Get()); cmd->Dispatch(1, 1, 1);
            flags[0] = Barrier(j.disabled.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            flags[1] = Barrier(j.flag12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            cmd->ResourceBarrier(2, flags); cmd->CopyBufferRegion(j.readback.Get(), 0, j.disabled.Get(), 0, 4);
            flags[0] = Barrier(j.disabled.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); cmd->ResourceBarrier(1, flags);
        }
        // Motion/depth were already restored; only colour resources remain.
        for (UINT i = 0; i != 3; ++i) std::swap(resources[i].Transition.StateBefore, resources[i].Transition.StateAfter);
        cmd->ResourceBarrier(3, resources);
        if (FAILED(cmd->Close())) return Fail("generation command close");
        ID3D12CommandList* lists[]{cmd}; queue->ExecuteCommandLists(1, lists);
        j.ready = ++ready_value; j.warmup = reset;
        if (FAILED(queue->Signal(ready12.Get(), j.ready))) return Fail("ready GPU signal");
        ++counters.submitted; return true;
    }
};

NativeBackend::NativeBackend() = default;
NativeBackend::~NativeBackend() {
    // Hooks/NGX driver callbacks can outlive native profile teardown. Bounded
    // session graphs remain pinned until process exit; never block LS or unload
    // a driver module while an independent engine may still reference it.
    (void)state_.release();
}
const char* NativeBackend::FailureStep() const { return state_ ? state_->step : "not initialized"; }
uint32_t NativeBackend::ErrorCode() const { return state_ ? state_->code : 0; }
HRESULT NativeBackend::Initialize(ID3D11DeviceContext* context, const TextureObservation& source,
    const BackendOptions& options, const std::filesystem::path& folder) {
    if (state_ || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !source.width || !source.height ||
        source.width > 16384 || source.height > 16384 || !FormatSupported(static_cast<DXGI_FORMAT>(source.format)) ||
        source.mip || source.samples != 1 || source.array_size != 1 || source.view_format != source.format) return E_INVALIDARG;
    state_ = std::make_unique<State>(); auto& s = *state_; s.options = options; s.extent = source;
    s.options.quality = std::clamp(options.quality, 1u, 5u); s.options.analysis_percent = std::clamp(options.analysis_percent, 10u, 100u);
    s.slots = std::clamp(options.slots, 2u, 4u);
    if (!std::isfinite(options.motion_scale) || std::abs(options.motion_scale) > 8) { s.Fail("motion scale"); return E_INVALIDARG; }
#ifdef LS_NATIVE_TEST
    if (options.synthetic_test) s.options.optical_flow = false;
#endif
    ComPtr<ID3D11Device> native; context->GetDevice(&native);
    HRESULT hr = native.As(&s.native_device); if (FAILED(s.Check(hr, "D3D11 device5"))) return hr;
    hr = context->QueryInterface(IID_PPV_ARGS(&s.native_context)); if (FAILED(s.Check(hr, "D3D11 context4"))) return hr;
    ComPtr<IDXGIDevice> dxgi; ComPtr<IDXGIAdapter> adapter; DXGI_ADAPTER_DESC ad{};
    if (FAILED(hr = native.As(&dxgi)) || FAILED(hr = dxgi->GetAdapter(&adapter)) || FAILED(hr = adapter->GetDesc(&ad))) return s.Check(hr, "native adapter");
    bool synthetic = false;
#ifdef LS_NATIVE_TEST
    synthetic = options.synthetic_test;
#endif
    if (!synthetic && ad.VendorId != 0x10de) { s.Fail("LS adapter is not NVIDIA"); return E_INVALIDARG; }
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s.device)); if (FAILED(s.Check(hr, "D3D12 same adapter"))) return hr;
    if (Luid(ad.AdapterLuid) != Luid(s.device->GetAdapterLuid())) { s.Fail("adapter LUID mismatch"); return E_INVALIDARG; }
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(hr = s.device->CreateCommandQueue(&q, IID_PPV_ARGS(&s.queue)))) return s.Check(hr, "backend queue");
    if (FAILED(hr = s.Fences())) return s.Check(hr, "shared fences");
    s.analysis_width = std::max(1u, (source.width * s.options.analysis_percent + 99) / 100);
    s.analysis_height = std::max(1u, (source.height * s.options.analysis_percent + 99) / 100);
    if (s.options.optical_flow && !s.OpticalFlow()) return E_FAIL;
    if (FAILED(hr = s.Shaders())) return s.Check(hr, "own preprocessing/composite shaders");
    for (UINT i = 0; i != s.slots; ++i) if (FAILED(hr = s.JobResources(s.jobs[i]))) return s.Check(hr, "preallocated job resources");
    if (!synthetic && !s.NgxInit(folder)) return E_FAIL;
    s.initialized = true; s.step = "ready"; return S_OK;
}
bool NativeBackend::Source(ID3D11Texture2D* current, const SourceWriteObservation& write) {
    if (!state_) return false;
    auto& s = *state_; std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    if (s.failed || !s.initialized || !current || !SourceWriteValid(write)) return false;
    D3D11_TEXTURE2D_DESC desc{}; current->GetDesc(&desc); ComPtr<ID3D11Device> device; current->GetDevice(&device);
    if (write.adapter_luid != Luid(s.device->GetAdapterLuid()) ||
        write.texture.width != desc.Width || write.texture.height != desc.Height || write.texture.format != static_cast<uint32_t>(desc.Format) ||
        !Same(device.Get(), s.native_device.Get()) || desc.Width != s.extent.width || desc.Height != s.extent.height ||
        static_cast<uint32_t>(desc.Format) != s.extent.format || desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        desc.MiscFlags != 0 || desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags != 0) return false;
    const bool pair = s.last_source && CanContinueSource(s.last_write, write) &&
        s.last_write.epoch == write.epoch && s.last_write.generation + 1 == write.generation;
    auto previous = s.last_write; ComPtr<ID3D11Texture2D> previous_source = s.last_source;
    s.last_write = write; s.last_source = current;
    if (!pair) return false;
    const auto ready = s.ready12->GetCompletedValue(), copied = s.copied11->GetCompletedValue();
    if (ready == UINT64_MAX || copied == UINT64_MAX) return s.Fail("device removed while acquiring job");
    State::Job* job = nullptr;
    for (UINT i = 0; i != s.slots; ++i) {
        auto& candidate = s.jobs[i];
        if (ready >= candidate.ready && (!candidate.consumed || copied >= candidate.copied)) { job = &candidate; break; }
    }
    if (!job) { ++s.counters.busy; return false; }
    auto& j = *job; j.previous = previous; j.current = write; j.consumed = false;
    s.native_context->CopyResource(j.inputs11[0].Get(), previous_source.Get());
    s.native_context->CopyResource(j.inputs11[1].Get(), current);
    if (FAILED(s.native_context->Signal(s.producer11.Get(), ++s.producer_value))) return s.Fail("native producer signal");
    LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
    const bool reset = s.last_evaluated_epoch != write.epoch || s.last_evaluated_generation + 1 != write.generation ||
        write.qpc - previous.qpc > static_cast<uint64_t>(frequency.QuadPart) / 4;
    if (!s.Generate(j, reset)) return false;
    s.last_evaluated_epoch = write.epoch; s.last_evaluated_generation = write.generation;
    return true;
}
bool NativeBackend::Composite(const DispatchObservation& slot, ID3D11UnorderedAccessView* destination) {
    if (!state_ || !destination) return false;
    auto& s = *state_; std::unique_lock<std::mutex> lock(s.mutex, std::try_to_lock); if (!lock.owns_lock()) return false;
    if (s.failed || !s.initialized || !slot.pair_matches_observed_updates || !slot.constants_known || slot.phase_bits != 0x3f000000u ||
        slot.output.mip || slot.output.width != s.extent.width || slot.output.height != s.extent.height ||
        slot.output.samples != 1 || slot.output.array_size != 1 || !FormatSupported(static_cast<DXGI_FORMAT>(slot.output.view_format))) return false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc{}; destination->GetDesc(&view_desc);
    ComPtr<ID3D11UnorderedAccessView> bound;
    s.native_context->CSGetUnorderedAccessViews(0, 1, &bound);
    if (!Same(bound.Get(), destination)) return false;
    ComPtr<ID3D11Resource> resource; destination->GetResource(&resource); ComPtr<ID3D11Device> device; resource->GetDevice(&device);
    if (!Same(device.Get(), s.native_device.Get()) || view_desc.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D || view_desc.Texture2D.MipSlice != 0) return false;
    ComPtr<ID3D11Texture2D> texture; D3D11_TEXTURE2D_DESC desc{};
    if (FAILED(resource.As(&texture))) return false;
    texture->GetDesc(&desc);
    if (desc.Width != slot.output.width || desc.Height != slot.output.height || static_cast<uint32_t>(desc.Format) != slot.output.format ||
        static_cast<uint32_t>(view_desc.Format) != slot.output.view_format || desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1) return false;
    State::Job* job = nullptr;
    for (UINT i = 0; i != s.slots; ++i) {
        auto& j = s.jobs[i];
        if (j.ready && !j.consumed && !j.warmup && SameSourceWrite(j.previous, slot.previous_write) && SameSourceWrite(j.current, slot.current_write)) { job = &j; break; }
    }
    if (!job) { ++s.counters.mismatched; return false; }
    auto& j = *job;
    const auto completed = s.ready12->GetCompletedValue(); if (completed == UINT64_MAX) return s.Fail("removed before composite");
    if (!s.options.gpu_ordered && completed < j.ready) { ++s.counters.busy; return false; }
    ComPtr<ID3D11ComputeShader> shader; UINT classes = 0; s.native_context->CSGetShader(&shader, nullptr, &classes);
    if (classes) return false;
    ID3D11ShaderResourceView* saved_raw[2]{}; s.native_context->CSGetShaderResources(0, 2, saved_raw);
    ComPtr<ID3D11ShaderResourceView> saved[2]; saved[0].Attach(saved_raw[0]); saved[1].Attach(saved_raw[1]);
    if (FAILED(s.native_context->Wait(s.ready11.Get(), j.ready))) return s.Fail("native ready GPU dependency");
    ID3D11ShaderResourceView* views[]{j.output_view.Get(), j.flag_view.Get()};
    s.native_context->CSSetShaderResources(0, 2, views); s.native_context->CSSetShader(s.composite_shader.Get(), nullptr, 0);
    s.native_context->Dispatch((s.extent.width + 15) / 16, (s.extent.height + 15) / 16, 1);
    // UAV, constants, samplers, other SRVs and every graphics binding are untouched.
    s.native_context->CSSetShaderResources(0, 2, saved_raw); s.native_context->CSSetShader(shader.Get(), nullptr, 0);
    j.copied = ++s.copy_value; j.consumed = true;
    if (FAILED(s.native_context->Signal(s.copied11.Get(), j.copied))) return s.Fail("native composite retirement");
    ++s.counters.composite_queued; return true;
}
BackendCounters NativeBackend::PollCounters() {
    if (!state_) return {};
    auto& s = *state_; std::lock_guard<std::mutex> lock(s.mutex);
    if (s.failed || !s.initialized) return s.counters;
    const auto completed = s.ready12->GetCompletedValue(); if (completed == UINT64_MAX) { s.Fail("removed while polling"); return s.counters; }
    for (UINT i = 0; i != s.slots; ++i) {
        auto& j = s.jobs[i]; if (!j.ready || j.ready <= j.polled || completed < j.ready) continue;
        void* pointer = nullptr; const D3D12_RANGE range{0, 4};
        if (SUCCEEDED(j.readback->Map(0, &range, &pointer)) && pointer) {
            uint32_t disabled = 1; std::memcpy(&disabled, pointer, 4); const D3D12_RANGE written{0, 0}; j.readback->Unmap(0, &written);
            if (disabled) ++s.counters.gpu_disabled; else ++s.counters.gpu_enabled; j.polled = j.ready;
        }
    }
    return s.counters;
}
} // namespace ls_native
