#include "observation_win.h"
#include "../include/observation.h"
#include "../include/source_pair.h"
#ifdef LS_WITH_NGX
#include "../backend/native_backend.h"
#include "policy_config.h"
#include "../include/ls_dispatch_contract.h"
#include <d3dcompiler.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <tlhelp32.h>
#endif
#include <MinHook.h>
#include <bcrypt.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#ifdef LS_OBSERVER_TEST
#include <iostream>
#include <condition_variable>
#include <sstream>
#include <d3dcompiler.h>
#endif

namespace {
using namespace ls_native;
constexpr char kDllHash[] = "626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb";
constexpr char kShaderHash[] = "3af0031e97f43a9372c23749ebbbe91506119268d84e869ba715ffe71bb9d45a";
constexpr char kInputShaderHash[] = "214a8ad496c0ffff58e9739d4be085a450d82eaaf23e2ad21dea8b251872880f";
constexpr GUID kSessionTag = {0x85697408,0xdd49,0x4383,{0x85,0xfb,0x4b,0x1e,0x43,0x5d,0x36,0x2d}};
constexpr GUID kSourceTag = {0x50df9543,0x14a1,0x4bbd,{0x92,0xa4,0x17,0x2b,0x6e,0x9c,0x41,0x25}};
constexpr GUID kHistoryTag = {0x672e5e3c,0x0a87,0x4e53,{0xbb,0xa6,0x9c,0xe6,0xdf,0x13,0x26,0x6b}};
constexpr GUID kShaderTag = {0x31cc6f57,0xb3af,0x46b2,{0x83,0x5a,0x15,0xa7,0xd3,0x1d,0x68,0x08}};
constexpr GUID kConstantTag = {0x8962412c,0x5b5a,0x445e,{0xaf,0x61,0xf6,0x56,0x5c,0x90,0x26,0xa5}};
constexpr GUID kDeviceTag = {0x0836a18d,0x2a0b,0x40fb,{0x8d,0xc7,0xc9,0x15,0xc0,0x91,0x01,0x14}};
constexpr GUID kObjectTag = {0x7e57029e,0x5cdb,0x4dbe,{0xbc,0xe9,0x13,0x08,0x74,0x34,0xf4,0xc6}};
struct DeviceTag { uint64_t object = 0, luid = 0; };
template<class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    T** Put() { return &p; }
    T* operator->() const { return p; }
};
template<class T, class Value> bool ReadTag(T* object, const GUID& guid, Value& value) {
    UINT size = sizeof(Value);
    return object && SUCCEEDED(object->GetPrivateData(guid, &size, &value)) && size == sizeof(Value);
}

struct Hash {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> storage;
    ~Hash() { if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    bool Open() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) return false;
        ULONG count = 0, written = 0;
        if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&count), sizeof(count), &written, 0)) return false;
        storage.resize(count);
        return BCryptCreateHash(alg, &hash, storage.data(), count, nullptr, 0, 0) == 0;
    }
    bool Add(const void* bytes, size_t size) {
        if (size > UINT32_MAX) return false;
        return BCryptHashData(hash, const_cast<PUCHAR>(static_cast<const UCHAR*>(bytes)), static_cast<ULONG>(size), 0) == 0;
    }
    std::string Finish() {
        std::array<UCHAR, 32> digest{};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0)) return {};
        constexpr char digits[] = "0123456789abcdef";
        std::string text;
        for (const auto byte : digest) { text += digits[byte >> 4]; text += digits[byte & 15]; }
        return text;
    }
};
std::string HashFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Hash hash;
    if (!file || !hash.Open()) return {};
    std::array<char, 65536> data{};
    while (file) {
        file.read(data.data(), data.size());
        if (file.gcount() && !hash.Add(data.data(), static_cast<size_t>(file.gcount()))) return {};
    }
    return file.eof() ? hash.Finish() : std::string{};
}

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using CreateShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**);
using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using CopyFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
using RegionFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
using UpdateFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using Region1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*, UINT);
using Update1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using IndirectFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using ResolveFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
using ClearUintFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const UINT*);
using ClearFloatFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const FLOAT*);
using DiscardResourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11Resource*);
using DiscardViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*);
using ClearViewFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*, const FLOAT*, const D3D11_RECT*, UINT);
using DiscardView1Fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3D11View*, const D3D11_RECT*, UINT);
struct ConstantTag { ConstantBytes bytes; uint64_t command_epoch = 0; uint32_t usage = 0; };
template<class Fn> struct Hook { void* target = nullptr; Fn original = nullptr; };
constexpr int kHookLimit = 12;
enum class HookKind : uint32_t {
    None, Dispatch, Indirect, Map, Copy, Region, Update, Execute, Resolve,
    ClearUint, ClearFloat, Region1, Update1, DiscardResource, DiscardView,
    ClearView, DiscardView1, Buffer, Shader
};
enum class StopReason : uint32_t {
    None, MissingCreate, MissingImport, MinHookInit, PatchImport, DeviceTag,
    HookInstall, CreateException, StartupException, TraceComplete, RecordLimit
};
template<class Fn> bool Install(Hook<Fn>(&hooks)[kHookLimit], void* target, HookKind kind);

// Pinned, process-lifetime observer. Hooks and trampolines are never freed while
// a native thread could be returning through them. No shutdown work in DllMain.
struct State {
    std::atomic<bool> active{false};
    std::atomic<bool> tracing{false}, eligible{false};
    std::atomic<uint64_t> serial{0}, seen{0}, untagged{0}, busy{0}, tagged_shaders{0}, tagged_buffers{0}, callbacks{0};
    // Packed records keep reason/code coherent without taking a render lock.
    std::atomic<uint64_t> stop_event{0}, hook_failure{0}, hook_failures{0};
    std::atomic<uint64_t> create_calls{0}, attached_devices{0}, dispatch_calls{0};
    std::atomic<uint32_t> create_hr{0};
    const uint64_t started_ms = GetTickCount64();
    std::mutex capture_mutex, install_mutex;
    ObservationQueue<DispatchObservation, 512> records;
    std::vector<uint8_t> shader, input_shader;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> analysis_shaders;
    uintptr_t native_base = 0;
    std::atomic<uint64_t> source_invalidation_epoch{1};
    std::ofstream log;
    Hook<CreateShaderFn> shaders[kHookLimit];
    Hook<CreateBufferFn> buffers[kHookLimit];
    Hook<DispatchFn> dispatches[kHookLimit];
    Hook<MapFn> maps[kHookLimit];
    Hook<CopyFn> copies[kHookLimit];
    Hook<RegionFn> regions[kHookLimit];
    Hook<UpdateFn> updates[kHookLimit];
    Hook<Region1Fn> regions1[kHookLimit];
    Hook<Update1Fn> updates1[kHookLimit];
    Hook<ExecuteFn> executions[kHookLimit];
    Hook<IndirectFn> indirects[kHookLimit];
    Hook<ResolveFn> resolves[kHookLimit];
    Hook<ClearUintFn> clear_uints[kHookLimit];
    Hook<ClearFloatFn> clear_floats[kHookLimit];
    Hook<DiscardResourceFn> discard_resources[kHookLimit];
    Hook<DiscardViewFn> discard_views[kHookLimit];
    Hook<ClearViewFn> clear_views[kHookLimit];
    Hook<DiscardView1Fn> discard_views1[kHookLimit];
    std::atomic<uint64_t> command_epoch{0};
    CreateDeviceFn create = nullptr;
    uint32_t duration = 20, max_records = 4096;
    uint64_t frequency = 0;
#ifdef LS_WITH_NGX
    std::atomic<bool> runtime_modules_written{false};
    struct Session {
        uint64_t context = 0, adapter_luid = 0, last_source_ms = 0, reattachments = 0;
        uint64_t last_attempt_context = 0, last_attempt_ms = 0;
        bool initialized = false;
        std::atomic<uint64_t> binding_context{0};
        std::atomic<uint32_t> render_mode{1}, prime{0};
        std::atomic<bool> final_verified{false};
        Com<ID3D11ComputeShader> repeat_shader;
        TextureObservation extent;
        std::unique_ptr<NativeBackend> backend;
    };
    BackendOptions options;
    RenderPolicy policy;
    std::mutex policy_mutex;
    std::atomic<uint32_t> requested_mode{1};
    std::atomic<uint64_t> analysis_skipped{0}, synthesis_skipped{0}, repeat_queued{0}, bypass_rejected{0}, unknown_analysis_call{0};
    std::atomic<uint32_t> hotkeys{0};
#ifdef LS_OBSERVER_TEST
    uint32_t test_return_rva = 0; // Synthetic fixture only; absent from the DLL.
#endif
    std::filesystem::path folder;
    std::mutex backend_mutex;
    std::array<Session, 4> sessions;
    std::ofstream backend_log;
    std::atomic<uint64_t> source_calls{0}, slots_seen{0}, profile_revision{0}, backend_busy{0}, session_limit{0};
    std::atomic<uint64_t> session_reused{0}, reattach_pending{0}, reattach_failed{0};
    std::atomic<uint64_t> latest_source_context{0}, selected_context{0};
    std::atomic<uint32_t> selection{0}; // 0 none, 1 selected, 2 capacity, 3 controller busy, 4 GPU pending, 5 init failed.
    std::atomic<int> profile_type{0}, profile_mode{0}, profile_hdr{0};
    std::atomic<float> profile_multiplier{0};
    std::atomic<uint32_t> backend_phase{0}; // 0 idle, 1 Initialize, 2 Source, 3 Composite, 4 Reattach.
    bool backend_writer_started = false; // Startup thread only.
#endif
};
State* g = nullptr;
void StopObservation(StopReason reason, uint32_t code = 0) {
    uint64_t empty = 0;
    const uint64_t event = uint64_t(static_cast<uint32_t>(reason)) << 32 | code;
    g->stop_event.compare_exchange_strong(empty, event); // Retain the first stop cause.
    g->active.store(false);
}
void RecordHookFailure(HookKind kind, uint32_t stage, uint32_t code) {
    g->hook_failure.store(uint64_t(static_cast<uint32_t>(kind)) << 48 | uint64_t(stage) << 32 | code);
    ++g->hook_failures;
}
#ifdef LS_WITH_NGX
const char* HookName(HookKind kind) {
    switch (kind) {
    case HookKind::Dispatch: return "Dispatch";
    case HookKind::Indirect: return "DispatchIndirect";
    case HookKind::Map: return "Map";
    case HookKind::Copy: return "CopyResource";
    case HookKind::Region: return "CopySubresourceRegion";
    case HookKind::Update: return "UpdateSubresource";
    case HookKind::Execute: return "ExecuteCommandList";
    case HookKind::Resolve: return "ResolveSubresource";
    case HookKind::ClearUint: return "ClearUnorderedAccessViewUint";
    case HookKind::ClearFloat: return "ClearUnorderedAccessViewFloat";
    case HookKind::Region1: return "CopySubresourceRegion1";
    case HookKind::Update1: return "UpdateSubresource1";
    case HookKind::DiscardResource: return "DiscardResource";
    case HookKind::DiscardView: return "DiscardView";
    case HookKind::ClearView: return "ClearView";
    case HookKind::DiscardView1: return "DiscardView1";
    case HookKind::Buffer: return "CreateBuffer";
    case HookKind::Shader: return "CreateComputeShader";
    default: return "none";
    }
}
const char* StopName(StopReason reason) {
    switch (reason) {
    case StopReason::MissingCreate: return "missing_d3d11_create";
    case StopReason::MissingImport: return "missing_delay_import";
    case StopReason::MinHookInit: return "minhook_initialize";
    case StopReason::PatchImport: return "delay_import_patch";
    case StopReason::DeviceTag: return "device_tag";
    case StopReason::HookInstall: return "hook_install";
    case StopReason::CreateException: return "device_create_exception";
    case StopReason::StartupException: return "startup_exception";
    case StopReason::TraceComplete: return "trace_complete";
    case StopReason::RecordLimit: return "record_limit";
    default: return "none";
    }
}
const char* BackendPhaseName(uint32_t phase) {
    switch (phase) {
    case 1: return "initializing";
    case 2: return "source";
    case 3: return "composite";
    case 4: return "reattaching";
    default: return "idle";
    }
}
struct BackendActivity {
    ~BackendActivity() { g->backend_phase.store(0); }
};
#endif
thread_local bool t_dispatch = false, t_shader = false, t_buffer = false;
#ifdef LS_WITH_NGX
bool BackendEnabled() { return g->options.enabled && g->eligible.load(); }
State::Session* FindSession(uint64_t context, const TextureObservation& extent) {
    for (auto& session : g->sessions)
        if (session.backend && session.context == context && session.extent.width == extent.width &&
            session.extent.height == extent.height && session.extent.format == extent.format) return &session;
    return nullptr;
}
const char* SelectionName(uint32_t value) {
    switch (value) {
    case 1: return "session_available";
    case 2: return "no_session_capacity";
    case 3: return "controller_busy";
    case 4: return "reattach_pending";
    case 5: return "initialization_failed";
    default: return "none";
    }
}
template<class T> uint64_t ObjectId(T* object);
State::Session* BoundSession(ID3D11DeviceContext* ctx) {
    State::Session* session = nullptr;
    if (!ReadTag(ctx, kSessionTag, session) || !session) return nullptr;
    // Reattachment publishes its identity last; an old native context cannot
    // use a graph now bound to a newer context.
    Com<ID3D11Device> device; ctx->GetDevice(device.Put());
    return session->binding_context.load() == ObjectId(ctx) ? session : nullptr;
}
bool RepeatNative(ID3D11DeviceContext* ctx, State::Session& session) {
    if (!session.repeat_shader.p) return false;
    Com<ID3D11ShaderResourceView> view; ctx->CSGetShaderResources(1, 1, view.Put());
    Com<ID3D11UnorderedAccessView> output; ctx->CSGetUnorderedAccessViews(0, 1, output.Put());
    if (!view.p || !output.p) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; view->GetDesc(&vd);
    D3D11_UNORDERED_ACCESS_VIEW_DESC od{}; output->GetDesc(&od);
    Com<ID3D11Resource> source, destination; view->GetResource(source.Put()); output->GetResource(destination.Put());
    Com<ID3D11Texture2D> a, b;
    if (source.p == destination.p || FAILED(source->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(a.Put()))) ||
        FAILED(destination->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(b.Put())))) return false;
    D3D11_TEXTURE2D_DESC ad{}, bd{}; a->GetDesc(&ad); b->GetDesc(&bd);
    if (vd.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || od.ViewDimension != D3D11_UAV_DIMENSION_TEXTURE2D ||
        vd.Texture2D.MostDetailedMip != 0 || vd.Texture2D.MipLevels != 1 || od.Texture2D.MipSlice != 0 ||
        ad.Width != bd.Width || ad.Height != bd.Height || ad.Format != bd.Format || vd.Format != ad.Format || od.Format != bd.Format ||
        ad.MipLevels != 1 || bd.MipLevels != 1 || ad.ArraySize != 1 || bd.ArraySize != 1 || ad.SampleDesc.Count != 1 || bd.SampleDesc.Count != 1) return false;
    Com<ID3D11ComputeShader> shader; UINT classes = 0; ctx->CSGetShader(shader.Put(), nullptr, &classes); if (classes) return false;
    ctx->CSSetShader(session.repeat_shader.p, nullptr, 0);
    ctx->Dispatch((bd.Width + 15) / 16, (bd.Height + 15) / 16, 1);
    ctx->CSSetShader(shader.p, nullptr, 0); ++g->repeat_queued; return true;
}
bool PrepareRepeat(ID3D11DeviceContext* ctx, State::Session& session) {
    // Independent per-binding fallback. Logger locks cannot prevent filling a
    // midpoint after native analysis was bypassed. No frame buffers or waits.
    constexpr char code[] = "Texture2D<float4> source:register(t1);RWTexture2D<float4> output:register(u0);"
        "[numthreads(16,16,1)]void main(uint3 p:SV_DispatchThreadID){uint w,h;output.GetDimensions(w,h);"
        "if(p.x<w && p.y<h)output[p.xy]=source.Load(int3(p.xy,0));}";
    Com<ID3DBlob> bytes, error;
    if (FAILED(D3DCompile(code, sizeof(code), nullptr, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, bytes.Put(), error.Put()))) return false;
    Com<ID3D11Device> device; ctx->GetDevice(device.Put());
    if (session.repeat_shader.p) { session.repeat_shader.p->Release(); session.repeat_shader.p = nullptr; }
    return SUCCEEDED(device->CreateComputeShader(bytes->GetBufferPointer(), bytes->GetBufferSize(), nullptr, session.repeat_shader.Put()));
}
void BackendSource(ID3D11DeviceContext* ctx, ID3D11Resource* source, const SourceWriteObservation& write) {
    if (!BackendEnabled() || !SourceWriteValid(write)) return;
    ++g->source_calls;
    g->latest_source_context.store(write.context);
    if (g->requested_mode.load() == 0 && !BoundSession(ctx)) return;
    std::unique_lock<std::mutex> lock(g->backend_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        ++g->backend_busy; ++g->source_invalidation_epoch;
        g->selected_context.store(0); g->selection.store(3); return;
    }
    BackendActivity activity;
    g->backend_phase.store(2);
    Com<ID3D11Texture2D> texture;
    if (!source || FAILED(source->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.Put())))) return;
    auto* session = FindSession(write.context, write.texture);
    const uint64_t now = GetTickCount64();
    bool pending = false;
    if (!session) for (auto& candidate : g->sessions) {
        // GPU retirement alone does not imply inactivity. Keep concurrently
        // producing contexts and incompatible extents/adapters in their slots.
        if (!candidate.backend || !candidate.initialized || candidate.adapter_luid != write.adapter_luid ||
            candidate.extent.width != write.texture.width || candidate.extent.height != write.texture.height ||
            candidate.extent.format != write.texture.format || now < candidate.last_source_ms ||
            now - candidate.last_source_ms < 1000) continue;
        if (candidate.last_attempt_context == write.context && now - candidate.last_attempt_ms < 250) continue;
        g->backend_phase.store(4);
        const HRESULT hr = candidate.backend->Reattach(ctx, write.texture);
        g->backend_phase.store(2);
        if (hr == S_OK) {
            candidate.context = write.context; candidate.extent = write.texture;
            candidate.last_attempt_context = candidate.last_attempt_ms = 0;
            ++candidate.reattachments; ++g->session_reused;
            session = &candidate; PrepareRepeat(ctx, candidate);
            candidate.final_verified.store(false); candidate.binding_context.store(write.context);
            ctx->SetPrivateData(kSessionTag, sizeof(session), &session); break;
        }
        if (hr == S_FALSE) { pending = true; ++g->reattach_pending; }
        else {
            ++g->reattach_failed; candidate.last_attempt_context = write.context; candidate.last_attempt_ms = now;
        }
    }
    if (!session) for (auto& candidate : g->sessions) {
        if (candidate.backend) continue;
        candidate.context = write.context; candidate.extent = write.texture; candidate.adapter_luid = write.adapter_luid;
        candidate.backend = std::make_unique<NativeBackend>();
        // One startup allocation on the native owning thread. No LS device is
        // touched by the logger, including SINGLETHREADED devices.
        g->backend_phase.store(1);
        candidate.initialized = SUCCEEDED(candidate.backend->Initialize(ctx, write.texture, g->options, g->folder));
        if (candidate.initialized) PrepareRepeat(ctx, candidate);
        candidate.final_verified.store(false); candidate.binding_context.store(write.context);
        auto* binding = &candidate; ctx->SetPrivateData(kSessionTag, sizeof(binding), &binding);
        g->backend_phase.store(2);
        session = &candidate; break;
    }
    if (session) {
        session->last_source_ms = GetTickCount64();
        g->selected_context.store(session->context); g->selection.store(session->initialized ? 1 : 5);
        RenderPolicy policy;
        { std::unique_lock<std::mutex> settings(g->policy_mutex, std::try_to_lock);
          if (!settings.owns_lock()) return;
          policy = g->policy; }
        policy.mode = static_cast<RenderMode>(g->requested_mode.load());
        if (!session->backend->SetPolicy(policy)) return;
        const auto prior = static_cast<RenderMode>(session->render_mode.exchange(static_cast<uint32_t>(policy.mode)));
        if (prior != policy.mode) {
            ++g->source_invalidation_epoch;
            if (prior == RenderMode::Economy) session->prime.store(2);
        }
        if (policy.mode != RenderMode::Economy && session->prime.load()) --session->prime;
        if (policy.mode != RenderMode::Native) session->backend->Source(texture.p, write);
    } else {
        ++g->session_limit; g->selected_context.store(0); g->selection.store(pending ? 4 : 2);
    }
}
bool BackendComposite(ID3D11DeviceContext* ctx, const DispatchObservation& record, bool before) {
    if (!BackendEnabled()) return false;
    auto* session = BoundSession(ctx); if (!session || !session->initialized) return false;
    const auto mode = static_cast<RenderMode>(session->render_mode.load());
    const bool fill = mode == RenderMode::Economy || session->prime.load() != 0;
    if (before != fill || (!fill && mode == RenderMode::Native)) return false;
    ++g->slots_seen;
    BackendActivity activity; g->backend_phase.store(3);
    Com<ID3D11UnorderedAccessView> destination; ctx->CSGetUnorderedAccessViews(0, 1, destination.Put());
    if (session->backend->Composite(record, destination.p, session->prime.load() != 0)) return true;
    if (fill && RepeatNative(ctx, *session)) return true;
    if (fill) ++g->bypass_rejected;
    return false;
}
bool BypassAnalysis(ID3D11DeviceContext* ctx, uint32_t rva) {
    if (!BackendEnabled()) return false;
    auto* session = BoundSession(ctx);
    if (!session || !session->initialized || !session->repeat_shader.p ||
        session->render_mode.load() != static_cast<uint32_t>(RenderMode::Economy) || !session->final_verified.load()) return false;
    Com<ID3D11ComputeShader> shader; ctx->CSGetShader(shader.Put(), nullptr, nullptr);
    uint32_t id = 0; if (!ReadTag(shader.p, kShaderTag, id) || !IsAnalysisResource(id)) return false;
    if (!IsAnalysisReturn(rva)) { ++g->unknown_analysis_call; return false; }
    ++g->analysis_skipped; return true;
}
void WriteRuntimeModules(std::ostream& out) {
    // Read only our own modules at startup/on the worker; never load a module
    // merely to inspect it. Include SM86/NGX, not unrelated process inventory.
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        out << "runtime_modules_error=" << GetLastError() << '\n'; return;
    }
    MODULEENTRY32W module{}; module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module)) do {
        bool relevant = false;
        for (const wchar_t* name : {L"version.dll", L"dxgi.dll", L"d3d12.dll", L"_nvngx.dll", L"nvngx.dll", L"nvngx_dlssg.dll", L"sm86_backend.dll"})
            relevant |= _wcsicmp(module.szModule, name) == 0;
        if (relevant) out << "runtime_module=" << std::quoted(std::filesystem::path(module.szExePath).u8string()) << '\n';
    } while (Module32NextW(snapshot, &module));
    CloseHandle(snapshot);
}
void PrepareSm86(std::ostream& out, const std::filesystem::path& folder) {
    // The supplied LS archive's utility proxy may have lost automatic loading
    // when the addon manager was removed. Explicit full-path loading also works
    // if .NET has already loaded System32/version.dll. Unknown DLLs are only
    // reported. This is outside DllMain and precedes every native NGX call.
    constexpr char known_version[] = "c3934a09399f022504227c72df0bf8c0de55f9a08880dddde898c5262cefa838";
    bool recognized = false;
    for (const wchar_t* relative : {L"version.dll", L"dxgi.dll", L"dlssg_sm86.ini", L"native-runtime/nvngx_dlssg.dll", L"addons/LS_DLSSFG/runtime/nvngx_dlssg.dll", L"nvngx_dlssg.dll"}) {
        const auto path = folder / relative;
        std::error_code error; const bool exists = std::filesystem::is_regular_file(path, error);
        out << "runtime_file=" << std::quoted(std::filesystem::path(relative).u8string()) << " exists=" << exists;
        const auto hash = exists ? HashFile(path) : std::string{};
        if (exists) out << " sha256=" << hash;
        out << '\n';
        if (std::wstring(relative) == L"version.dll" && hash == known_version) {
            recognized = true;
            const auto absolute = std::filesystem::absolute(path);
            const HMODULE loaded = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            const DWORD code = loaded ? 0 : GetLastError();
            out << "sm86_bootstrap=" << (loaded ? "loaded" : "load_failed") << " error=" << code;
            if (loaded) {
                wchar_t actual[32768]{};
                if (GetModuleFileNameW(loaded, actual, 32768)) out << " path=" << std::quoted(std::filesystem::path(actual).u8string());
                out << " proxy_export=" << (GetProcAddress(loaded, "DlssgProxy_Name") != nullptr);
                // Keep the load reference until process exit: its hooks and
                // NVIDIA callbacks must never point into an unloaded module.
            }
            out << '\n';
        }
    }
    if (!recognized) out << "sm86_bootstrap=no_recognized_utility_proxy\n";
    WriteRuntimeModules(out); out.flush();
}
void WriteBackendDiagnostics(std::ostream& out) {
    struct SessionSnapshot {
        uint64_t context = 0, last_source_ms = 0, reattachments = 0;
        TextureObservation extent;
        NativeBackend* backend = nullptr;
        BackendDiagnostics diagnostics;
        bool sampled = false;
    };
    std::array<SessionSnapshot, 4> snapshots;
    bool controller_sampled = false;
    {
        std::unique_lock<std::mutex> lock(g->backend_mutex, std::try_to_lock);
        controller_sampled = lock.owns_lock();
        if (controller_sampled) {
            for (size_t i = 0; i != snapshots.size(); ++i) {
                snapshots[i].context = g->sessions[i].context;
                snapshots[i].last_source_ms = g->sessions[i].last_source_ms;
                snapshots[i].reattachments = g->sessions[i].reattachments;
                snapshots[i].extent = g->sessions[i].extent;
                snapshots[i].backend = g->sessions[i].backend.get();
            }
        }
    }
    // Session pointers are process-lifetime pinned. No controller lock during
    // GPU-counter polling, formatting or file I/O. Busy backends skip a sample.
    for (auto& snapshot : snapshots)
        if (snapshot.backend) snapshot.sampled = snapshot.backend->TryPollDiagnostics(snapshot.diagnostics);
    out << "profile=" << g->profile_revision.load() << " type=" << g->profile_type.load()
        << " mode=" << g->profile_mode.load() << " multiplier=" << g->profile_multiplier.load()
        << " hdr=" << g->profile_hdr.load() << " eligible=" << g->eligible.load()
        << " source=" << g->source_calls.load() << " matched_slots=" << g->slots_seen.load()
        << " controller_busy=" << g->backend_busy.load() << " session_limit=" << g->session_limit.load()
        << " requested_mode=" << RenderModeName(static_cast<RenderMode>(g->requested_mode.load()))
        << " analysis_skipped=" << g->analysis_skipped.load() << " synthesis_skipped=" << g->synthesis_skipped.load()
        << " repeat_queued=" << g->repeat_queued.load() << " bypass_rejected=" << g->bypass_rejected.load()
        << " unknown_analysis_call=" << g->unknown_analysis_call.load() << " hotkeys=" << g->hotkeys.load()
        << " backend_phase=" << BackendPhaseName(g->backend_phase.load())
        << " diagnostics_busy=" << !controller_sampled << "\n";
    out << "routing latest_source_context=" << g->latest_source_context.load()
        << " selected_context=" << g->selected_context.load() << " selection=" << SelectionName(g->selection.load())
        << " session_reused=" << g->session_reused.load() << " reattach_pending=" << g->reattach_pending.load()
        << " reattach_failed=" << g->reattach_failed.load() << '\n';
    const auto stop = g->stop_event.load(), failure = g->hook_failure.load();
    const auto hook_stage = static_cast<uint32_t>(failure >> 32) & 0xffff;
    out << "observer uptime_ms=" << GetTickCount64() - g->started_ms << " active=" << g->active.load()
        << " stop=" << StopName(static_cast<StopReason>(stop >> 32))
        << " stop_code=0x" << std::hex << static_cast<uint32_t>(stop) << std::dec
        << " create_calls=" << g->create_calls.load() << " create_hr=0x" << std::hex << g->create_hr.load() << std::dec
        << " attached_devices=" << g->attached_devices.load() << " dispatch_callbacks=" << g->dispatch_calls.load()
        << " tagged_shaders=" << g->tagged_shaders.load() << " tagged_buffers=" << g->tagged_buffers.load()
        << " observed_dispatches=" << g->seen.load() << " untagged=" << g->untagged.load()
        << " hook_failures=" << g->hook_failures.load() << " hook=" << HookName(static_cast<HookKind>(failure >> 48))
        << " hook_stage=" << (hook_stage == 1 ? "create" : hook_stage == 2 ? "enable" : hook_stage == 3 ? "capacity" : "none")
        << " hook_code=" << static_cast<int32_t>(failure)
        << " hook_status=" << (hook_stage == 1 || hook_stage == 2 ? MH_StatusToString(static_cast<MH_STATUS>(static_cast<int32_t>(failure))) : "none")
        << "\n";
    for (const auto& snapshot : snapshots) if (snapshot.backend) {
        out << "session=" << snapshot.context << " " << snapshot.extent.width << 'x' << snapshot.extent.height;
        if (!snapshot.sampled) { out << " diagnostics_busy=1\n"; continue; }
        const auto& d = snapshot.diagnostics; const auto& c = d.counters;
        // A reattachment can occur after the controller snapshot but before
        // sampling. Do not attribute the new graph binding to the old context.
        if (d.reattachments != snapshot.reattachments) { out << " diagnostics_stale=1\n"; continue; }
        out << " step=" << d.step << " code=0x" << std::hex << d.code << std::dec
            << " reattachments=" << d.reattachments << " reattach_code=0x" << std::hex << d.reattach_code << std::dec
            << " source_age_ms=" << GetTickCount64() - snapshot.last_source_ms
            << " analysis=" << d.analysis_width << 'x' << d.analysis_height
            << " adapter_luid=0x" << std::hex << d.adapter_luid << std::dec
            << " adapter=" << std::quoted(std::filesystem::path(d.adapter_name.data()).u8string())
            << " render_mode=" << RenderModeName(d.mode) << " analysis_format=" << d.analysis_format << " flow_grid=" << d.grid
            << " submitted=" << c.submitted << " composite_queued=" << c.composite_queued
            << " busy=" << c.busy << " unmatched=" << c.mismatched << " flag_enabled_samples=" << c.gpu_enabled
            << " flag_disabled_samples=" << c.gpu_disabled << " failures=" << c.failed
            << " repeat_queued=" << c.repeat_queued << " duplicate_samples=" << c.duplicate_samples << " scene_cut_samples=" << c.scene_cut_samples
            << " scene_resets=" << c.scene_resets << " timing_samples=" << d.timing_samples
            << " conversion_gpu_ms=" << d.conversion_ms << " flow_dependency_gpu_ms=" << d.flow_dependency_ms << " generation_gpu_ms=" << d.generation_ms
            << " ineligible=" << c.ineligible << " destination_rejected=" << c.destination_rejected << " warmup=" << c.warmup << "\n";
        if (d.ngx.init) {
            const auto& n = d.ngx;
            out << "ngx session=" << snapshot.context << " init=0x" << std::hex << n.init << " parameters=0x" << n.parameters;
            const auto value = [&](const char* name, const NgxValue& v) {
                out << ' ' << name << "_query=0x" << std::hex << v.result << ' ' << name << "=0x" << static_cast<uint32_t>(v.value);
            };
            value("available", n.available); value("feature_init", n.feature_init); value("needs_driver", n.needs_driver);
            value("min_driver_major", n.min_driver_major); value("min_driver_minor", n.min_driver_minor);
            out << std::dec << '\n';
            if (!g->runtime_modules_written.exchange(true)) WriteRuntimeModules(out);
        }
    }
    NgxLogMessage message;
    for (size_t i = 0; i != 256 && NativeBackend::TryPopNgxLog(message); ++i)
        out << "ngx_log level=" << message.level << " feature=" << message.feature << " truncated=" << message.truncated
            << " message=" << std::quoted(message.text.data()) << '\n';
    if (const auto dropped = NativeBackend::DroppedNgxLogs()) out << "ngx_log_dropped=" << dropped << '\n';
    out.flush();
}
[[maybe_unused]] void RunBackendWriter(std::ostream& out) {
    for (;;) {
        WriteBackendDiagnostics(out);
        if (!g->active.load()) {
            out << "writer=stopped\n"; out.flush();
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
void BackendWriter() {
    // All D3D11 work remains on LS's owning thread. This thread only posts policy
    // changes and writes completed D3D12 diagnostic metadata.
    uint32_t registered = 0;
    for (int i = 0; i != 3; ++i)
        if (RegisterHotKey(nullptr, 0x4c50 + i, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F6 + static_cast<UINT>(i))) registered |= 1u << i;
    g->hotkeys.store(registered);
    uint64_t next = 0;
    while (g->active.load()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
            if (message.wParam == 0x4c50) {
                g->requested_mode.store((g->requested_mode.load() + 1) % 3); ++g->source_invalidation_epoch;
            } else if (message.wParam == 0x4c51) ++g->source_invalidation_epoch;
            else if (message.wParam == 0x4c52) {
                const auto config = ReadPolicyConfig(g->folder / L"NativeDLSS.ini");
                { std::lock_guard<std::mutex> lock(g->policy_mutex); g->policy = config.policy; }
                g->requested_mode.store(static_cast<uint32_t>(config.policy.mode)); ++g->source_invalidation_epoch;
                g->backend_log << "control=reload profile=" << std::quoted(std::filesystem::path(config.profile).u8string())
                    << " requested_mode=" << RenderModeName(config.policy.mode) << " gpu_quality_requires_restart=1\n";
            }
        }
        const auto now = GetTickCount64();
        if (now >= next) { WriteBackendDiagnostics(g->backend_log); next = now + 1000; }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    }
    for (int i = 0; i != 3; ++i) if (registered & (1u << i)) UnregisterHotKey(nullptr, 0x4c50+i);
    WriteBackendDiagnostics(g->backend_log); g->backend_log << "writer=stopped\n"; g->backend_log.flush();
}
// Startup thread only, before the periodic writer owns this stream.
void StartupNote(const char* stage) {
    if (!g->backend_writer_started && g->backend_log) {
        g->backend_log << "startup=" << stage << "\n"; g->backend_log.flush();
    }
}

#endif
template<class T> uint64_t ObjectId(T* object) {
    uint64_t value = 0;
    if (ReadTag(object, kObjectTag, value)) return value;
    value = ++g->serial;
    return SUCCEEDED(object->SetPrivateData(kObjectTag, sizeof(value), &value)) ? value : 0;
}

void Texture(ID3D11Resource* resource, UINT view_format, UINT mip, TextureObservation& out) {
    if (!resource) return;
    Com<ID3D11Texture2D> texture;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.Put())))) return;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    out.object = ObjectId(resource);
    out.width = desc.Width; out.height = desc.Height; out.format = desc.Format;
    out.view_format = view_format; out.mip = mip; out.array_size = desc.ArraySize;
    out.samples = desc.SampleDesc.Count; out.bind_flags = desc.BindFlags;
}

void Observe(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z, bool after_dispatch, DispatchObservation* captured = nullptr) {
    Com<ID3D11ComputeShader> shader;
    ctx->CSGetShader(shader.Put(), nullptr, nullptr);
    uint32_t resource_id = 0;
    if (!ReadTag(shader.p, kShaderTag, resource_id) ||
        (after_dispatch ? resource_id != 254 : resource_id != 256)) return;
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { ++g->untagged; return; }
    const uint64_t sequence = ++g->seen;
    if (g->tracing.load() && sequence > g->max_records) {
        g->tracing.store(false);
#ifdef LS_WITH_NGX
        if (!g->options.enabled) StopObservation(StopReason::RecordLimit);
#else
        StopObservation(StopReason::RecordLimit);
#endif
    }
    std::unique_lock<std::mutex> lock(g->capture_mutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++g->busy; ++g->source_invalidation_epoch; return; }
    Com<ID3D11Device> device;
    ctx->GetDevice(device.Put());
    DeviceTag tag;
    if (!ReadTag(device.p, kDeviceTag, tag)) { ++g->untagged; return; }
    DispatchObservation record;
    record.input_update = after_dispatch;
    record.sequence = sequence; record.device = tag.object; record.adapter_luid = tag.luid;
    record.context = ObjectId(ctx); record.thread = GetCurrentThreadId();
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now); record.qpc = static_cast<uint64_t>(now.QuadPart);
    record.groups_x = x; record.groups_y = y; record.groups_z = z;

    Com<ID3D11DeviceContext1> ctx1;
    Com<ID3D11Buffer> cb;
    UINT first = 0, count = 4096;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(ctx1.Put()))))
        ctx1->CSGetConstantBuffers1(0, 1, cb.Put(), &first, &count);
    else ctx->CSGetConstantBuffers(0, 1, cb.Put());
    ConstantTag constants;
    if (ReadTag(cb.p, kConstantTag, constants) &&
        (constants.usage == D3D11_USAGE_IMMUTABLE || constants.command_epoch == g->command_epoch.load()))
        DecodeConstants(constants.bytes, first, count, record);
    else { record.first_constant = first; record.num_constants = count; }

    std::array<SourceWriteObservation, 2> source_tags{};
    for (UINT i = 0; i != (after_dispatch ? 1u : 5u); ++i) {
        Com<ID3D11ShaderResourceView> view;
        ctx->CSGetShaderResources(i, 1, view.Put());
        if (!view.p) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; view->GetDesc(&desc);
        if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) continue;
        Com<ID3D11Resource> texture; view->GetResource(texture.Put());
        Texture(texture.p, desc.Format, desc.Texture2D.MostDetailedMip, record.inputs[i]);
        if (i < source_tags.size()) ReadTag(texture.p, kSourceTag, source_tags[i]);
    }
    Com<ID3D11Resource> output_resource;
    Com<ID3D11UnorderedAccessView> output;
    ctx->CSGetUnorderedAccessViews(0, 1, output.Put());
    if (output.p) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{}; output->GetDesc(&desc);
        if (desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D) {
            output->GetResource(output_resource.Put());
            Texture(output_resource.p, desc.Format, desc.Texture2D.MipSlice, record.output);
        }
    }
    SourcePairHistory history;
    ReadTag(ctx, kHistoryTag, history);
    const uint64_t invalidation = g->source_invalidation_epoch.load();
    if (history.invalidation_epoch != invalidation ||
        history.current.command_epoch != g->command_epoch.load()) history = {};
    if (after_dispatch) {
        auto& w = record.source_write;
        w.sequence = sequence; w.qpc = record.qpc; w.command_epoch = g->command_epoch.load();
        w.device = record.device; w.context = record.context; w.adapter_luid = record.adapter_luid;
        w.thread = record.thread; w.texture = record.output;
        if (CanContinueSource(history.current, w)) {
            w.epoch = history.current.epoch; w.generation = history.current.generation + 1;
        } else { w.epoch = ++g->serial; w.generation = 1; }
        const bool valid = record.inputs[0].object && record.inputs[0].object != w.texture.object &&
            x == (uint64_t(w.texture.width) + 7) / 8 && y == (uint64_t(w.texture.height) + 7) / 8 && z == 1 &&
            AppendSourceWrite(history, w);
        history.invalidation_epoch = invalidation;
        if (!valid || !output_resource.p ||
            FAILED(output_resource->SetPrivateData(kSourceTag, sizeof(w), &w)) ||
            FAILED(ctx->SetPrivateData(kHistoryTag, sizeof(history), &history))) {
            if (output_resource.p) output_resource->SetPrivateData(kSourceTag, 0, nullptr);
            ++g->untagged; ++g->source_invalidation_epoch;
        }
#ifdef LS_WITH_NGX
        else BackendSource(ctx, output_resource.p, w);
#endif
    } else {
        record.previous_write = source_tags[0]; record.current_write = source_tags[1];
        record.pair_matches_observed_updates = MatchObservedSourcePair(history, source_tags[0], source_tags[1], record);
    }
    if (captured) *captured = record;
    if (g->tracing.load() && !g->records.TryPush(record)) ++g->source_invalidation_epoch;
}

template<int I> HRESULT STDMETHODCALLTYPE Shader(ID3D11Device* dev, const void* bytes, SIZE_T size,
    ID3D11ClassLinkage* linkage, ID3D11ComputeShader** result) {
    const auto original = g->shaders[I].original;
    if (t_shader || !g->active.load()) return original(dev, bytes, size, linkage, result);
    t_shader = true;
    const HRESULT hr = original(dev, bytes, size, linkage, result);
    DeviceTag device;
    if (SUCCEEDED(hr) && result && *result && bytes && ReadTag(dev, kDeviceTag, device)) {
        uint32_t id = 0;
        if (size == g->shader.size() && std::memcmp(bytes, g->shader.data(), size) == 0) id = 256;
        else if (size == g->input_shader.size() && std::memcmp(bytes, g->input_shader.data(), size) == 0) id = 254;
        else for (const auto& analysis : g->analysis_shaders)
            if (size == analysis.second.size() && std::memcmp(bytes, analysis.second.data(), size) == 0) { id = analysis.first; break; }
        if (id && SUCCEEDED((*result)->SetPrivateData(kShaderTag, sizeof(id), &id))) ++g->tagged_shaders;
    }
    t_shader = false;
    return hr;
}
template<int I> HRESULT STDMETHODCALLTYPE Buffer(ID3D11Device* dev, const D3D11_BUFFER_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* data, ID3D11Buffer** result) {
    const auto original = g->buffers[I].original;
    if (t_buffer || !g->active.load()) return original(dev, desc, data, result);
    t_buffer = true;
    // Copy while pSysMem belongs to this call. DYNAMIC bytes are valid only
    // until a tracked write/Map or deferred command-list execution invalidates them.
    ConstantTag snapshot;
    DeviceTag device;
    const bool capture = desc && data && data->pSysMem && ReadTag(dev, kDeviceTag, device) &&
        (desc->Usage == D3D11_USAGE_IMMUTABLE || desc->Usage == D3D11_USAGE_DYNAMIC) &&
        (desc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
        desc->ByteWidth >= 32 && desc->ByteWidth <= snapshot.bytes.bytes.size();
    if (capture) {
        snapshot.bytes.byte_width = desc->ByteWidth;
        snapshot.command_epoch = g->command_epoch.load(); snapshot.usage = desc->Usage;
        std::memcpy(snapshot.bytes.bytes.data(), data->pSysMem, desc->ByteWidth);
    }
    const HRESULT hr = original(dev, desc, data, result);
    if (capture && SUCCEEDED(hr) && result && *result &&
        SUCCEEDED((*result)->SetPrivateData(kConstantTag, sizeof(snapshot), &snapshot))) ++g->tagged_buffers;
    t_buffer = false;
    return hr;
}
void Invalidate(ID3D11Resource* destination) {
    if (!g->active.load() || !destination) return;
    ConstantTag prior;
    if (ReadTag(destination, kConstantTag, prior)) destination->SetPrivateData(kConstantTag, 0, nullptr);
    SourceWriteObservation source;
    if (ReadTag(destination, kSourceTag, source)) {
        destination->SetPrivateData(kSourceTag, 0, nullptr); ++g->source_invalidation_epoch;
    }
}
void InvalidateView(ID3D11View* view) {
    if (!view) return;
    Com<ID3D11Resource> resource; view->GetResource(resource.Put()); Invalidate(resource.p);
}
template<int I> void STDMETHODCALLTYPE Resolve(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub,
    ID3D11Resource* src, UINT src_sub, DXGI_FORMAT format) {
    Invalidate(dst); g->resolves[I].original(ctx, dst, sub, src, src_sub, format);
}
template<int I> void STDMETHODCALLTYPE ClearUint(ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* view, const UINT* values) {
    InvalidateView(view); g->clear_uints[I].original(ctx, view, values);
}
template<int I> void STDMETHODCALLTYPE ClearFloat(ID3D11DeviceContext* ctx, ID3D11UnorderedAccessView* view, const FLOAT* values) {
    InvalidateView(view); g->clear_floats[I].original(ctx, view, values);
}
template<int I> void STDMETHODCALLTYPE DiscardResource(ID3D11DeviceContext1* ctx, ID3D11Resource* resource) {
    Invalidate(resource); g->discard_resources[I].original(ctx, resource);
}
template<int I> void STDMETHODCALLTYPE DiscardView(ID3D11DeviceContext1* ctx, ID3D11View* view) {
    InvalidateView(view); g->discard_views[I].original(ctx, view);
}
template<int I> void STDMETHODCALLTYPE ClearView(ID3D11DeviceContext1* ctx, ID3D11View* view, const FLOAT* values, const D3D11_RECT* rects, UINT count) {
    InvalidateView(view); g->clear_views[I].original(ctx, view, values, rects, count);
}
template<int I> void STDMETHODCALLTYPE DiscardView1(ID3D11DeviceContext1* ctx, ID3D11View* view, const D3D11_RECT* rects, UINT count) {
    InvalidateView(view); g->discard_views1[I].original(ctx, view, rects, count);
}
template<int I> HRESULT STDMETHODCALLTYPE Map(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub,
    D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* result) {
    // Never read a native write-combined mapped pointer. Even a failed Map
    // conservatively makes our creation-time snapshot unavailable.
    Invalidate(res); return g->maps[I].original(ctx, res, sub, type, flags, result);
}
template<int I> void STDMETHODCALLTYPE Copy(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src) {
    Invalidate(dst); g->copies[I].original(ctx, dst, src);
}
template<int I> void STDMETHODCALLTYPE Region(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub,
    UINT x, UINT y, UINT z, ID3D11Resource* src, UINT src_sub, const D3D11_BOX* box) {
    Invalidate(dst); g->regions[I].original(ctx, dst, sub, x, y, z, src, src_sub, box);
}
template<int I> void STDMETHODCALLTYPE Update(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT sub,
    const D3D11_BOX* box, const void* data, UINT pitch, UINT depth) {
    Invalidate(dst); g->updates[I].original(ctx, dst, sub, box, data, pitch, depth);
}
template<int I> void STDMETHODCALLTYPE Region1(ID3D11DeviceContext1* ctx, ID3D11Resource* dst, UINT sub,
    UINT x, UINT y, UINT z, ID3D11Resource* src, UINT src_sub, const D3D11_BOX* box, UINT flags) {
    Invalidate(dst); g->regions1[I].original(ctx, dst, sub, x, y, z, src, src_sub, box, flags);
}
template<int I> void STDMETHODCALLTYPE Update1(ID3D11DeviceContext1* ctx, ID3D11Resource* dst, UINT sub,
    const D3D11_BOX* box, const void* data, UINT pitch, UINT depth, UINT flags) {
    Invalidate(dst); g->updates1[I].original(ctx, dst, sub, box, data, pitch, depth, flags);
}
template<int I> void STDMETHODCALLTYPE Execute(ID3D11DeviceContext* ctx, ID3D11CommandList* list, BOOL restore) {
    if (g->active.load()) ++g->command_epoch; // Contents of arbitrary lists are unknown.
    g->executions[I].original(ctx, list, restore);
}
// Conservative invalidation for compute writes to previously tagged inputs.
// Graphics Draw/OM UAV and external writes are NOT covered; logs explicitly
// refuse to certify source contents or authorize replacement from these stamps.
void InvalidateComputeOutputs(ID3D11DeviceContext* ctx) {
    Com<ID3D11Device> device; ctx->GetDevice(device.Put()); DeviceTag tag;
    if (!ReadTag(device.p, kDeviceTag, tag)) return;
    Com<ID3D11ComputeShader> shader; ctx->CSGetShader(shader.Put(), nullptr, nullptr);
    uint32_t id = 0; ReadTag(shader.p, kShaderTag, id);
    const UINT count = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1 ? D3D11_1_UAV_SLOT_COUNT : D3D11_PS_CS_UAV_REGISTER_COUNT;
    std::array<ID3D11UnorderedAccessView*, D3D11_1_UAV_SLOT_COUNT> raw{};
    std::array<Com<ID3D11UnorderedAccessView>, D3D11_1_UAV_SLOT_COUNT> views;
    ctx->CSGetUnorderedAccessViews(0, count, raw.data());
    for (UINT i = 0; i != count; ++i) views[i].p = raw[i];
    for (UINT i = 0; i != count; ++i) {
        const auto& view = views[i];
        if (!view.p) continue;
        Com<ID3D11Resource> resource; view->GetResource(resource.Put());
        SourceWriteObservation prior;
        if (!ReadTag(resource.p, kSourceTag, prior)) continue;
        if (id == 254 && i == 0) resource->SetPrivateData(kSourceTag, 0, nullptr);
        else Invalidate(resource.p);
    }
}
template<int I> void STDMETHODCALLTYPE Indirect(ID3D11DeviceContext* ctx, ID3D11Buffer* args, UINT offset) {
    if (g->active.load() && !t_dispatch) {
        InvalidateComputeOutputs(ctx);
        // Indirect dimensions are GPU data; never infer source continuity.
        Com<ID3D11Device> device; ctx->GetDevice(device.Put()); DeviceTag tag;
        if (ReadTag(device.p, kDeviceTag, tag)) ++g->source_invalidation_epoch;
    }
    g->indirects[I].original(ctx, args, offset);
}
template<int I> void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    const auto original = g->dispatches[I].original;
#ifdef LS_WITH_NGX
#ifdef _MSC_VER
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
#else
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
#endif
    uint32_t rva = caller >= g->native_base && caller - g->native_base < 0x379ff ? static_cast<uint32_t>(caller - g->native_base) : 0;
#ifdef LS_OBSERVER_TEST
    if (g->test_return_rva) rva = g->test_return_rva;
#endif
#endif
    if (t_dispatch) { original(ctx, x, y, z); return; }
    ++g->callbacks; ++g->dispatch_calls;
    if (g->active.load()) {
        t_dispatch = true;
        #ifdef LS_WITH_NGX
        if (BypassAnalysis(ctx, rva)) { t_dispatch = false; --g->callbacks; return; }
#endif
        DispatchObservation record;
        try { InvalidateComputeOutputs(ctx); Observe(ctx, x, y, z, false, &record); }
        catch (...) { ++g->untagged; ++g->source_invalidation_epoch; }
        bool replaced = false;
#ifdef LS_WITH_NGX
        // Only the exact REA-confirmed final call may be replaced before LS work.
        if (rva == kSynthesisReturn) {
            try { replaced = BackendComposite(ctx, record, true); } catch (...) { ++g->bypass_rejected; }
            if (auto* session = BoundSession(ctx)) session->final_verified.store(replaced);
        }
#endif
        if (!replaced) original(ctx, x, y, z);
#ifdef LS_WITH_NGX
        else ++g->synthesis_skipped;
#endif
        try {
            if (g->active.load()) {
#ifdef LS_WITH_NGX
                if (!replaced) BackendComposite(ctx, record, false);
#endif
                Observe(ctx, x, y, z, true);
            }
        }
        catch (...) { ++g->untagged; ++g->source_invalidation_epoch; }
        t_dispatch = false;
    } else original(ctx, x, y, z);
    --g->callbacks;
}
template<class Fn, int... I> std::array<void*, kHookLimit> Detours(std::integer_sequence<int,I...>) {
    if constexpr (std::is_same_v<Fn, DispatchFn>) return {reinterpret_cast<void*>(&Dispatch<I>)...};
    else if constexpr (std::is_same_v<Fn, CreateShaderFn>) return {reinterpret_cast<void*>(&Shader<I>)...};
    else if constexpr (std::is_same_v<Fn, CreateBufferFn>) return {reinterpret_cast<void*>(&Buffer<I>)...};
    else if constexpr (std::is_same_v<Fn, MapFn>) return {reinterpret_cast<void*>(&Map<I>)...};
    else if constexpr (std::is_same_v<Fn, CopyFn>) return {reinterpret_cast<void*>(&Copy<I>)...};
    else if constexpr (std::is_same_v<Fn, RegionFn>) return {reinterpret_cast<void*>(&Region<I>)...};
    else if constexpr (std::is_same_v<Fn, UpdateFn>) return {reinterpret_cast<void*>(&Update<I>)...};
    else if constexpr (std::is_same_v<Fn, Region1Fn>) return {reinterpret_cast<void*>(&Region1<I>)...};
    else if constexpr (std::is_same_v<Fn, Update1Fn>) return {reinterpret_cast<void*>(&Update1<I>)...};
    else if constexpr (std::is_same_v<Fn, IndirectFn>) return {reinterpret_cast<void*>(&Indirect<I>)...};
    else if constexpr (std::is_same_v<Fn, ResolveFn>) return {reinterpret_cast<void*>(&Resolve<I>)...};
    else if constexpr (std::is_same_v<Fn, ClearUintFn>) return {reinterpret_cast<void*>(&ClearUint<I>)...};
    else if constexpr (std::is_same_v<Fn, ClearFloatFn>) return {reinterpret_cast<void*>(&ClearFloat<I>)...};
    else if constexpr (std::is_same_v<Fn, DiscardResourceFn>) return {reinterpret_cast<void*>(&DiscardResource<I>)...};
    else if constexpr (std::is_same_v<Fn, DiscardViewFn>) return {reinterpret_cast<void*>(&DiscardView<I>)...};
    else if constexpr (std::is_same_v<Fn, ClearViewFn>) return {reinterpret_cast<void*>(&ClearView<I>)...};
    else if constexpr (std::is_same_v<Fn, DiscardView1Fn>) return {reinterpret_cast<void*>(&DiscardView1<I>)...};
    else return {reinterpret_cast<void*>(&Execute<I>)...};
}

bool InstallContext(ID3D11DeviceContext* ctx) {
    void** table = *reinterpret_cast<void***>(ctx);
    bool complete = Install(g->dispatches, table[41], HookKind::Dispatch);
    complete &= Install(g->indirects, table[42], HookKind::Indirect);
    complete &= Install(g->maps, table[14], HookKind::Map);
    complete &= Install(g->copies, table[47], HookKind::Copy);
    complete &= Install(g->regions, table[46], HookKind::Region);
    complete &= Install(g->updates, table[48], HookKind::Update);
    complete &= Install(g->executions, table[58], HookKind::Execute);
    complete &= Install(g->resolves, table[57], HookKind::Resolve);
    complete &= Install(g->clear_uints, table[51], HookKind::ClearUint);
    complete &= Install(g->clear_floats, table[52], HookKind::ClearFloat);
    Com<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(ctx1.Put())))) {
        void** extended = *reinterpret_cast<void***>(ctx1.p);
        complete &= Install(g->regions1, extended[115], HookKind::Region1);
        complete &= Install(g->updates1, extended[116], HookKind::Update1);
        complete &= Install(g->discard_resources, extended[117], HookKind::DiscardResource);
        complete &= Install(g->discard_views, extended[118], HookKind::DiscardView);
        complete &= Install(g->clear_views, extended[132], HookKind::ClearView);
        complete &= Install(g->discard_views1, extended[133], HookKind::DiscardView1);
    }
    return complete;
}
template<class Fn> bool Install(Hook<Fn>(&hooks)[kHookLimit], void* target, HookKind kind) {
    for (const auto& hook : hooks) if (hook.target == target) return true;
    for (int i = 0; i != kHookLimit; ++i) {
        if (hooks[i].target) continue;
        const auto detours = Detours<Fn>(std::make_integer_sequence<int,kHookLimit>{});
        void* trampoline = nullptr;
        const auto created = MH_CreateHook(target, detours[i], &trampoline);
        if (created != MH_OK) { RecordHookFailure(kind, 1, static_cast<uint32_t>(created)); return false; }
        static_assert(sizeof(trampoline) == sizeof(hooks[i].original), "Windows function pointer ABI");
        std::memcpy(&hooks[i].original, &trampoline, sizeof(trampoline));
        hooks[i].target = target; // Published before any native thread can enter.
        const auto enabled = MH_EnableHook(target);
        if (enabled == MH_OK) return true;
        RecordHookFailure(kind, 2, static_cast<uint32_t>(enabled));
        MH_RemoveHook(target); hooks[i] = {};
        return false;
    }
    RecordHookFailure(kind, 3, ERROR_TOO_MANY_CMDS);
    return false;
}

void Attach(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    std::lock_guard<std::mutex> lock(g->install_mutex); // Once per device creation, not Dispatch.
    DeviceTag tag;
    tag.object = ++g->serial;
    Com<IDXGIDevice> dxgi; Com<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.Put()))) &&
        SUCCEEDED(dxgi->GetAdapter(adapter.Put())) && SUCCEEDED(adapter->GetDesc(&desc))) {
        tag.luid = uint64_t(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32 | desc.AdapterLuid.LowPart;
    }
    const HRESULT tagged = dev->SetPrivateData(kDeviceTag, sizeof(tag), &tag);
    if (FAILED(tagged)) { ++g->untagged; StopObservation(StopReason::DeviceTag, static_cast<uint32_t>(tagged)); return; }
    void** table = *reinterpret_cast<void***>(dev);
    const bool context_complete = InstallContext(ctx);
    const bool buffer_complete = Install(g->buffers, table[3], HookKind::Buffer);
    const bool shader_complete = Install(g->shaders, table[18], HookKind::Shader);
    if (!context_complete || !buffer_complete || !shader_complete) {
        ++g->untagged;
        StopObservation(StopReason::HookInstall); // Incomplete observation cannot certify phases.
    }
    else ++g->attached_devices;
}
HRESULT WINAPI CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk, ID3D11Device** dev,
    D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    ++g->create_calls;
    const HRESULT hr = g->create(adapter, type, software, flags, levels, count, sdk, dev, level, ctx);
    g->create_hr.store(static_cast<uint32_t>(hr));
    try {
        if (g->active.load() && SUCCEEDED(hr) && dev && *dev) {
            if (ctx && *ctx) Attach(*dev, *ctx);
            else { Com<ID3D11DeviceContext> immediate; (*dev)->GetImmediateContext(immediate.Put()); Attach(*dev, immediate.p); }
        }
    } catch (...) { ++g->untagged; StopObservation(StopReason::CreateException); }
    return hr;
}

// Own bounded PE lookup. The exact native image was hash-checked before this is
// called. Only its D3D11CreateDevice delay-import slot is modified, not LS code.
void** DelayDeviceSlot(HMODULE module) {
    auto base = reinterpret_cast<uint8_t*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const uint32_t image_size = nt->OptionalHeader.SizeOfImage;
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    auto contains = [&](uint64_t offset, uint64_t size) { return offset <= image_size && size <= image_size - offset; };
    struct Delay { DWORD attributes, name, handle, iat, names, bound, unload, timestamp; };
    if (!contains(directory.VirtualAddress, directory.Size)) return nullptr;
    for (uint32_t pos = 0; pos + sizeof(Delay) <= directory.Size; pos += sizeof(Delay)) {
        auto entry = reinterpret_cast<Delay*>(base + directory.VirtualAddress + pos);
        if (!entry->name) break;
        if (!(entry->attributes & 1) || !contains(entry->name, 10) ||
            std::memcmp(base + entry->name, "d3d11.dll", 10)) continue;
        for (uint64_t i = 0; contains(uint64_t(entry->names) + i * 8, 8) && contains(uint64_t(entry->iat) + i * 8, 8); ++i) {
            uint64_t name = 0; std::memcpy(&name, base + entry->names + i * 8, 8);
            if (!name) break;
            if ((name & IMAGE_ORDINAL_FLAG64) || !contains(name, 20)) continue;
            if (!std::memcmp(base + name + 2, "D3D11CreateDevice", 18))
                return reinterpret_cast<void**>(base + entry->iat + i * 8);
        }
    }
    return nullptr;
}
bool PatchSlot(void** slot, void* hook) {
    DWORD old = 0;
    if (!slot || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(slot, hook);
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old, &ignored);
    return true;
}

void WriteTexture(std::ostream& out, const TextureObservation& t) {
    out << "{\"object\":" << t.object << ",\"width\":" << t.width << ",\"height\":" << t.height
        << ",\"format\":" << t.format << ",\"view_format\":" << t.view_format << ",\"mip\":" << t.mip
        << ",\"array_size\":" << t.array_size << ",\"samples\":" << t.samples << ",\"bind_flags\":" << t.bind_flags << '}';
}
void WriteSource(std::ostream& out, const SourceWriteObservation& w) {
    out << "{\"epoch\":" << w.epoch << ",\"generation\":" << w.generation
        << ",\"sequence\":" << w.sequence << ",\"qpc\":" << w.qpc << ",\"command_epoch\":" << w.command_epoch
        << ",\"device\":" << w.device << ",\"context\":" << w.context << ",\"adapter_luid\":" << w.adapter_luid
        << ",\"thread\":" << w.thread << ",\"texture\":";
    WriteTexture(out, w.texture); out << '}';
}
void WriteRecord(const DispatchObservation& r) {
    auto& out = g->log;
    out << "{\"kind\":\"" << (r.input_update ? "input_update" : "dispatch") << "\",\"sequence\":" << r.sequence << ",\"qpc\":" << r.qpc
        << ",\"device\":" << r.device << ",\"context\":" << r.context << ",\"adapter_luid\":" << r.adapter_luid
        << ",\"thread\":" << r.thread << ",\"groups\":[" << r.groups_x << ',' << r.groups_y << ',' << r.groups_z
        << "],\"constants_known\":" << (r.constants_known ? "true" : "false") << ",\"phase_bits\":" << r.phase_bits
        << ",\"resolution_scale_bits\":" << r.resolution_scale_bits << ",\"cb_byte_width\":" << r.cb_byte_width
        << ",\"first_constant\":" << r.first_constant << ",\"num_constants\":" << r.num_constants << ",\"inputs\":[";
    for (size_t i = 0; i != (r.input_update ? 1u : r.inputs.size()); ++i) { if (i) out << ','; WriteTexture(out, r.inputs[i]); }
    out << "],\"output\":"; WriteTexture(out, r.output);
    if (r.input_update) { out << ",\"source_write\":"; WriteSource(out, r.source_write); }
    else {
        out << ",\"pair_matches_observed_updates\":" << (r.pair_matches_observed_updates ? "true" : "false");
        out << ",\"previous_write\":"; WriteSource(out, r.previous_write);
        out << ",\"current_write\":"; WriteSource(out, r.current_write);
    }
    out << "}\n";
}
void Writer() {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(g->duration);
    uint64_t written = 0;
    while (g->active.load() && g->tracing.load() && std::chrono::steady_clock::now() < end && g->log) {
        DispatchObservation record;
        while (g->records.TryPop(record)) { WriteRecord(record); ++written; }
        g->log.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Worker only.
    }
    g->tracing.store(false);
#ifdef LS_WITH_NGX
    if (!g->options.enabled) StopObservation(StopReason::TraceComplete);
#else
    StopObservation(StopReason::TraceComplete);
#endif
    // Native observation callbacks might already be running: footer is written
    // after their capture critical section has retired, outside the render thread.
    std::lock_guard<std::mutex> lock(g->capture_mutex);
    DispatchObservation record;
    while (g->records.TryPop(record)) { WriteRecord(record); ++written; }
    g->log << "{\"kind\":\"footer\",\"written\":" << written << ",\"seen\":" << g->seen.load()
        << ",\"dropped_queue\":" << g->records.Dropped() << ",\"dropped_busy\":" << g->busy.load()
        << ",\"untagged\":" << g->untagged.load() << ",\"tagged_shaders\":" << g->tagged_shaders.load()
        << ",\"tagged_buffers\":" << g->tagged_buffers.load() << "}\n";
    g->log.close();
}
} // namespace

void StartNativeObservation(HMODULE native, HMODULE proxy, const std::filesystem::path& folder) noexcept {
    try {
        const auto ini = folder / L"NativeDLSS.ini";
        const bool tracing = GetPrivateProfileIntW(L"Observation", L"Enabled", 0, ini.c_str()) == 1;
        bool backend = false;
#ifdef LS_WITH_NGX
        backend = GetPrivateProfileIntW(L"NativeDLSS", L"Enabled", 0, ini.c_str()) == 1;
#endif
        if (!tracing && !backend) return;
        wchar_t nativePath[32768]{};
        const DWORD path_size = GetModuleFileNameW(native, nativePath, 32768);
        if (!path_size || path_size >= 32768 || HashFile(nativePath) != kDllHash) {
            OutputDebugStringW(L"Native DLSS observer: native DLL hash mismatch; leaving LS native.\n"); return;
        }
        const auto res = FindResourceW(native, MAKEINTRESOURCEW(256), MAKEINTRESOURCEW(10));
        const auto size = SizeofResource(native, res);
        const auto bytes = static_cast<const uint8_t*>(LockResource(LoadResource(native, res)));
        Hash sha;
        if (!bytes || !size || !sha.Open() || !sha.Add(bytes, size) || sha.Finish() != kShaderHash) return;
        const auto input_res = FindResourceW(native, MAKEINTRESOURCEW(254), MAKEINTRESOURCEW(10));
        const auto input_size = SizeofResource(native, input_res);
        const auto input_bytes = static_cast<const uint8_t*>(LockResource(LoadResource(native, input_res)));
        Hash input_sha;
        if (!input_bytes || !input_size || !input_sha.Open() || !input_sha.Add(input_bytes, input_size) ||
            input_sha.Finish() != kInputShaderHash) return;
        // Pin before installing executable detours. Never remove a trampoline
        // while native callers might still be inside it or unload this module.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(proxy), &pinned)) return;
        g = new State; // Deliberate process-lifetime storage; no loader-lock teardown.
        g->tracing.store(tracing);
#ifdef LS_WITH_NGX
        g->folder = folder;
        const auto config = ReadPolicyConfig(ini); g->options = config.backend; g->options.enabled = backend;
        g->policy = config.policy; g->requested_mode.store(static_cast<uint32_t>(config.policy.mode));
        g->native_base = reinterpret_cast<uintptr_t>(native);
        for (uint32_t id = 255; id <= 302; ++id) {
            if (!IsAnalysisResource(id)) continue;
            const auto item = FindResourceW(native, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
            const auto count = item ? SizeofResource(native, item) : 0;
            const auto data = item ? static_cast<const uint8_t*>(LockResource(LoadResource(native, item))) : nullptr;
            if (data && count) g->analysis_shaders.emplace_back(id, std::vector<uint8_t>(data, data + count));
        }
#endif
        g->shader.assign(bytes, bytes + size);
        g->input_shader.assign(input_bytes, input_bytes + input_size);
        g->duration = std::clamp(GetPrivateProfileIntW(L"Observation", L"DurationSeconds", 20, ini.c_str()), 1u, 120u);
        g->max_records = std::clamp(GetPrivateProfileIntW(L"Observation", L"MaxRecords", 4096, ini.c_str()), 1u, 65536u);
        LARGE_INTEGER frequency{}, start{};
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        g->frequency = static_cast<uint64_t>(frequency.QuadPart);
        std::filesystem::create_directories(folder / L"logs");
        if (tracing) {
          g->log.open(folder / L"logs" / (L"native-observation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(start.QuadPart) + L".jsonl"));
          if (!g->log) return;
          g->log << "{\"kind\":\"header\",\"schema_version\":2,\"dll_sha256\":\"" << kDllHash
            << "\",\"shader_sha256\":\"" << kShaderHash << "\",\"resource_id\":256,\"qpc_frequency\":" << g->frequency
            << ",\"input_shader_sha256\":\"" << kInputShaderHash << "\",\"input_resource_id\":254"
            << ",\"source_mutation_coverage_complete\":false,\"source_content_verified\":false"
            << ",\"present_owner\":\"LS\",\"replacement_enabled\":" << (backend ? "true" : "false") << ",\"source_frame_ids_known\":false}\n";
          g->log.flush();
        }
#ifdef LS_WITH_NGX
        if (backend) {
            g->backend_log.open(folder / L"logs" / (L"native-dlss-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(start.QuadPart) + L".log"));
            if (!g->backend_log) return;
            g->backend_log << "NativeDLSS 0.2.0 experimental. Present owner: LS. Full source coverage / real RTX3080 acceptance: unverified.\n"
                << "OpticalFlow=" << g->options.optical_flow << " Quality=" << g->options.quality
                << " AnalysisPercent=" << g->options.analysis_percent << " GPUOrdered=" << g->options.gpu_ordered
                << " Slots=" << g->options.slots << " FlowPreset=" << g->options.flow_preset << " FlowGrid=" << g->options.flow_grid
                << " requested_mode=" << RenderModeName(config.policy.mode) << " profile=" << std::quoted(std::filesystem::path(config.profile).u8string()) << "\n";
            PrepareSm86(g->backend_log, folder);
        }
#endif
        auto fail_startup = [](StopReason reason, uint32_t code) {
            StopObservation(reason, code);
#ifdef LS_WITH_NGX
            StartupNote("stopped");
            if (g->backend_log) WriteBackendDiagnostics(g->backend_log);
#endif
        };
#ifdef LS_WITH_NGX
        StartupNote("resolve_d3d11");
#endif
        const auto d3d = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        g->create = d3d ? NativeExport<CreateDeviceFn>(d3d, "D3D11CreateDevice") : nullptr;
        void** const slot = DelayDeviceSlot(native);
        if (!g->create) { fail_startup(StopReason::MissingCreate, GetLastError()); return; }
        if (!slot) { fail_startup(StopReason::MissingImport, ERROR_PROC_NOT_FOUND); return; }
        const auto initialized = MH_Initialize();
        if (initialized != MH_OK) { fail_startup(StopReason::MinHookInit, static_cast<uint32_t>(initialized)); return; }
#ifdef LS_WITH_NGX
        StartupNote("warp_probe_begin");
#endif
        // Resolve the runtime's normal and multithread-refresh Dispatch entries
        // on a WARP probe; no native LS bindings are changed for probing.
        for (const UINT flags : {0u, UINT(D3D11_CREATE_DEVICE_SINGLETHREADED)}) {
            Com<ID3D11Device> probe; Com<ID3D11DeviceContext> ctx;
            const HRESULT probe_hr = g->create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                D3D11_SDK_VERSION, probe.Put(), nullptr, ctx.Put());
#ifdef LS_WITH_NGX
            if (g->backend_log) {
                g->backend_log << "probe flags=" << flags << " hr=0x" << std::hex << static_cast<uint32_t>(probe_hr) << std::dec << "\n";
                g->backend_log.flush();
            }
#endif
            if (FAILED(probe_hr)) continue;
            auto note = [&] {
                InstallContext(ctx.p);
                ctx->CSSetShader(nullptr, nullptr, 0);
                InstallContext(ctx.p);
            };
            note();
            Com<ID3D11Multithread> mt;
            if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(mt.Put()))))
                for (const BOOL protect : {TRUE, FALSE, TRUE}) { mt->SetMultithreadProtected(protect); note(); }
        }
#ifdef LS_WITH_NGX
        StartupNote("warp_probe_complete");
#endif
        // The periodic writer also emits a terminal snapshot if activation
        // fails or a later native device cannot install its complete hooks.
        g->active.store(true);
        if (!PatchSlot(slot, reinterpret_cast<void*>(&CreateDevice))) StopObservation(StopReason::PatchImport, GetLastError());
#ifdef LS_WITH_NGX
        StartupNote(g->active.load() ? "native_create_hook_ready" : "native_create_hook_failed");
#endif
        if (tracing) std::thread(Writer).detach();
#ifdef LS_WITH_NGX
        if (backend) {
            std::thread writer(BackendWriter);
            g->backend_writer_started = true;
            writer.detach();
        }
#endif
    } catch (...) {
        if (g) {
            StopObservation(StopReason::StartupException);
#ifdef LS_WITH_NGX
            if (!g->backend_writer_started && g->backend_log) {
                StartupNote("exception"); WriteBackendDiagnostics(g->backend_log);
            }
#endif
        }
        OutputDebugStringW(L"Native DLSS observer failed; leaving LS native.\n");
    }
}

void SetNativeProfile(int type, int mode, float multiplier, bool hdr) noexcept {
    if (!g) return;
    ++g->source_invalidation_epoch;
#ifdef LS_WITH_NGX
    g->profile_type.store(type); g->profile_mode.store(mode); g->profile_multiplier.store(multiplier);
    g->profile_hdr.store(hdr); ++g->profile_revision;
    // Values verified in this exact LosslessScaling.dll's CLI enum constants:
    // FrameGenerationEnum.LSFG3=1, LSFG3ModeEnum.FIXED=0.
    g->eligible.store(type == 1 && mode == 0 && multiplier == 2.0f && !hdr);
#else
    (void)type; (void)mode; (void)multiplier; (void)hdr;
#endif
}

#ifdef LS_OBSERVER_TEST
// Uses only synthetic CPU constants and Windows WARP. No LS/NVIDIA payload.
// Exercises real runtime detours and rejects creation snapshots after mutation.
int NativeObservationSelfTest() {
    g = new State;
    g->create = &D3D11CreateDevice;
    if (MH_Initialize() != MH_OK) return 1;
    Com<ID3D11Device> dev; Com<ID3D11DeviceContext> ctx;
    if (FAILED(g->create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, dev.Put(), nullptr, ctx.Put()))) return 1;
    ctx->CSSetShader(nullptr, nullptr, 0);
    Attach(dev.p, ctx.p);
    if (g->untagged.load()) return 1;
    g->active.store(true);
    g->tracing.store(true);
#ifdef LS_WITH_NGX
    g->options.enabled = true; g->options.synthetic_test = true; g->folder = L".";
    SetNativeProfile(1, 0, 2.0f, false);
#endif
    std::array<uint32_t,12> bytes{}; bytes[7] = 0x3f000000;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 48; desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA initial{}; initial.pSysMem = bytes.data();
    Com<ID3D11Buffer> buffer;
    if (FAILED(dev->CreateBuffer(&desc, &initial, buffer.Put()))) return 1;
    ConstantTag tag;
    DispatchObservation observed;
    if (!ReadTag(buffer.p, kConstantTag, tag) || tag.usage != D3D11_USAGE_DYNAMIC ||
        !DecodeConstants(tag.bytes, 0, 4096, observed) || observed.phase_bits != bytes[7]) return 1;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(buffer.p, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return 1;
    const bool invalidated = !ReadTag(buffer.p, kConstantTag, tag);
    std::memcpy(mapped.pData, bytes.data(), sizeof(bytes));
    ctx->Unmap(buffer.p, 0);
    if (!invalidated) { std::cerr << "dynamic snapshot survived Map\n"; return 1; }
    Com<ID3D11Buffer> next;
    if (FAILED(dev->CreateBuffer(&desc, &initial, next.Put())) || !ReadTag(next.p, kConstantTag, tag)) return 1;
    const uint64_t initial_epoch = tag.command_epoch;
    Com<ID3D11DeviceContext> deferred; Com<ID3D11CommandList> list;
    if (FAILED(dev->CreateDeferredContext(0, deferred.Put()))) return 1;
    deferred->CSSetShader(nullptr, nullptr, 0);
    if (FAILED(deferred->FinishCommandList(FALSE, list.Put()))) return 1;
    ctx->ExecuteCommandList(list.p, FALSE);
    if (initial_epoch == g->command_epoch.load()) { std::cerr << "command-list provenance unchanged\n"; return 1; }
    // Compile our own two tiny kernels. No commercial shader bytes are used.
    auto compile = [&](const char* source, std::vector<uint8_t>& identity, ID3D11ComputeShader** result) {
        Com<ID3DBlob> code, error;
        if (FAILED(D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS, 0, code.Put(), error.Put()))) return false;
        const auto begin = static_cast<const uint8_t*>(code->GetBufferPointer());
        identity.assign(begin, begin + code->GetBufferSize());
        return SUCCEEDED(dev->CreateComputeShader(begin, code->GetBufferSize(), nullptr, result));
    };
    const char* copy_code =
        "Texture2D<float4> A : register(t0); RWTexture2D<float4> O : register(u0);"
        "[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID) { O[p.xy] = A.Load(int3(p.xy,0)); }";
    const char* synth_code =
        "Texture2D<float4> A : register(t0); Texture2D<float4> B : register(t1);"
        "RWTexture2D<float4> O : register(u0);"
        "[numthreads(16,16,1)] void main(uint3 p:SV_DispatchThreadID) {"
#ifdef LS_WITH_NGX
        " O[p.xy] = float4(1,0,1,1); }";
#else
        " O[p.xy] = (A.Load(int3(p.xy,0))+B.Load(int3(p.xy,0)))*0.5; }";
#endif
    Com<ID3D11ComputeShader> copy_shader, synth_shader;
    if (!compile(copy_code, g->input_shader, copy_shader.Put()) ||
        !compile(synth_code, g->shader, synth_shader.Put())) return 1;
    uint32_t id = 0;
    if (!ReadTag(copy_shader.p, kShaderTag, id) || id != 254 ||
        !ReadTag(synth_shader.p, kShaderTag, id) || id != 256) return 1;
    std::array<Com<ID3D11Texture2D>, 6> images;
    std::array<Com<ID3D11ShaderResourceView>, 6> views;
    std::array<Com<ID3D11UnorderedAccessView>, 6> outputs;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 16; td.Height = 16; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (size_t i = 0; i != images.size(); ++i) {
        if (FAILED(dev->CreateTexture2D(&td, nullptr, images[i].Put())) ||
            FAILED(dev->CreateShaderResourceView(images[i].p, nullptr, views[i].Put())) ||
            FAILED(dev->CreateUnorderedAccessView(images[i].p, nullptr, outputs[i].Put()))) return 1;
    }
    bytes[6] = 0x3f800000;
    Com<ID3D11Buffer> phase;
    if (FAILED(dev->CreateBuffer(&desc, &initial, phase.Put()))) return 1;
    ctx->CSSetConstantBuffers(0, 1, &phase.p);
    auto unbind = [&] {
        ID3D11ShaderResourceView* nulls[5]{}; ID3D11UnorderedAccessView* null_output = nullptr;
        ctx->CSSetShaderResources(0, 5, nulls); ctx->CSSetUnorderedAccessViews(0, 1, &null_output, nullptr);
    };
    auto update = [&](size_t destination) {
        unbind(); ctx->CSSetShader(copy_shader.p, nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &views[0].p);
        ctx->CSSetUnorderedAccessViews(0, 1, &outputs[destination].p, nullptr);
        ctx->Dispatch(2, 2, 1); unbind();
    };
    auto synthesize = [&](size_t previous, size_t current) {
        unbind(); ctx->CSSetShader(synth_shader.p, nullptr, 0);
        ID3D11ShaderResourceView* bindings[]{views[previous].p, views[current].p, views[0].p, views[3].p, views[4].p};
        ctx->CSSetShaderResources(0, 5, bindings);
        ctx->CSSetUnorderedAccessViews(0, 1, &outputs[5].p, nullptr);
        ctx->Dispatch(1, 1, 1); unbind();
    };
    DispatchObservation first, second, synthesis;
    update(1); update(2); synthesize(1, 2);
    if (!g->records.TryPop(first) || !g->records.TryPop(second) || !g->records.TryPop(synthesis) ||
        !first.input_update || !second.input_update || first.source_write.generation != 1 ||
        second.source_write.generation != 2 || synthesis.input_update ||
        !synthesis.pair_matches_observed_updates || !synthesis.constants_known) {
        std::cerr << "post-Dispatch source update pair was not established\n"; return 1;
    }
    synthesize(2, 1);
    if (!g->records.TryPop(synthesis) || synthesis.pair_matches_observed_updates) return 1;
    ctx->CopyResource(images[1].p, images[0].p);
    synthesize(1, 2);
    if (!g->records.TryPop(synthesis) || synthesis.pair_matches_observed_updates) {
        std::cerr << "pair survived an unaccounted CopyResource write\n"; return 1;
    }
    update(1); update(2); update(1); synthesize(2, 1);
    DispatchObservation third;
    if (!g->records.TryPop(first) || !g->records.TryPop(second) || !g->records.TryPop(third) ||
        !g->records.TryPop(synthesis) || first.source_write.generation != 1 ||
        third.source_write.generation != 3 || third.source_write.texture.object != first.source_write.texture.object ||
        !synthesis.pair_matches_observed_updates || g->untagged.load()) return 1;
    // Known kernel writing the same object twice cannot establish distinct sources.
    update(1); synthesize(2, 1);
    if (!g->records.TryPop(first) || !g->records.TryPop(synthesis) || synthesis.pair_matches_observed_updates) return 1;
    update(2); synthesize(1, 2);
    if (!g->records.TryPop(first) || !g->records.TryPop(synthesis) || !synthesis.pair_matches_observed_updates) return 1;
    const FLOAT clear[4]{}; ctx->ClearUnorderedAccessViewFloat(outputs[1].p, clear);
    synthesize(1, 2);
    if (!g->records.TryPop(synthesis) || synthesis.pair_matches_observed_updates) { std::cerr << "pair survived UAV clear\n"; return 1; }
#ifdef LS_WITH_NGX
    Com<ID3D11Device5> dev5; Com<ID3D11DeviceContext4> ctx4; Com<ID3D11Fence> fence;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(dev5.Put()))) ||
        FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(ctx4.Put()))) ||
        FAILED(dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void**>(fence.Put())))) return 1;
    uint64_t completion = 0;
    auto drain = [&] {
        if (FAILED(ctx4->Signal(fence.p, ++completion))) return false;
        ctx->Flush(); // TEST ONLY: the production hook path does not flush/wait.
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        const HRESULT hr = fence->SetEventOnCompletion(completion, event);
        const DWORD result = SUCCEEDED(hr) ? WaitForSingleObject(event, 15000) : WAIT_FAILED;
        CloseHandle(event); return result == WAIT_OBJECT_0;
    };
    auto source = [&](uint32_t color, size_t target) {
        unbind(); std::vector<uint32_t> pixels(256, color);
        ctx->UpdateSubresource(images[0].p, 0, nullptr, pixels.data(), 64, 0); update(target);
    };
    auto pixels = [&](uint32_t expected) {
        if (!drain()) return false;
        D3D11_TEXTURE2D_DESC read_desc = td; read_desc.BindFlags = 0;
        read_desc.Usage = D3D11_USAGE_STAGING; read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> read;
        if (FAILED(dev->CreateTexture2D(&read_desc, nullptr, read.Put()))) return false;
        ctx->CopyResource(read.p, images[5].p); if (!drain()) return false;
        D3D11_MAPPED_SUBRESOURCE mapped_image{};
        if (FAILED(ctx->Map(read.p, 0, D3D11_MAP_READ, 0, &mapped_image))) return false;
        bool valid = true;
        for (UINT y = 0; y != 16; ++y) for (UINT x = 0; x != 16; ++x) {
            uint32_t value = 0; std::memcpy(&value, static_cast<const uint8_t*>(mapped_image.pData) + y * mapped_image.RowPitch + x * 4, 4);
            valid &= value == expected;
        }
        ctx->Unmap(read.p, 0); return valid;
    };
    if (!drain()) return 1;
    SetNativeProfile(1, 0, 2.0f, false);
    source(0xff000014, 1); source(0xff00003c, 2); synthesize(1, 2);
    if (!pixels(0xffff00ff)) { std::cerr << "hook warmup did not preserve native output\n"; return 1; }
    source(0xff000064, 1); synthesize(2, 1);
    if (!pixels(0xff000050)) { std::cerr << "real Dispatch hook did not replace native magenta with pair midpoint\n"; return 1; }
    // A blocked diagnostic sink must not own either frame-path lock. Sampling
    // finishes before the first stream write, then the render thread continues.
    struct BlockedDiagnosticSink : std::streambuf {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, released = false;
        void Block() {
            std::unique_lock<std::mutex> lock(mutex);
            entered = true; changed.notify_all();
            changed.wait(lock, [&] { return released; });
        }
        std::streamsize xsputn(const char*, std::streamsize size) override { Block(); return size; }
        int_type overflow(int_type value) override { Block(); return traits_type::not_eof(value); }
        bool AwaitWrite() {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
        }
        void Release() {
            std::lock_guard<std::mutex> lock(mutex); released = true; changed.notify_all();
        }
    } sink;
    const auto busy_before = g->backend_busy.load(), epoch_before = g->source_invalidation_epoch.load();
    std::ostream blocked_log(&sink);
    std::thread logger([&] { WriteBackendDiagnostics(blocked_log); });
    const bool logger_blocked = sink.AwaitWrite();
    bool uninterrupted = false;
    if (logger_blocked) {
        source(0xff00008c, 2); synthesize(1, 2);
        uninterrupted = pixels(0xff000078) && g->backend_busy.load() == busy_before &&
            g->source_invalidation_epoch.load() == epoch_before;
    }
    sink.Release(); logger.join();
    if (!logger_blocked || !uninterrupted) {
        std::cerr << "blocked diagnostic output disrupted source history or composite\n"; return 1;
    }
    std::cout << "Blocked diagnostic output preserves source generations and GPU midpoint replacement\n";
    // Synthetic call-site injection exists only in this test translation unit.
    // It proves our bypass dispatcher and pixels, not the actual LS caller ABI.
    g->requested_mode.store(2); ++g->source_invalidation_epoch;
    g->test_return_rva = kSourceReturn;
    source(0xff000014,1); source(0xff00003c,2);
    g->test_return_rva = kSynthesisReturn; synthesize(1,2);
    if (!pixels(0xff00003c)) { std::cerr << "economy warmup source fill\n"; return 1; }
    g->test_return_rva = kSourceReturn; source(0xff000064,1);
    g->test_return_rva = kSynthesisReturn; synthesize(2,1);
    if (!pixels(0xff000050)) { std::cerr << "economy generated midpoint\n"; return 1; }
    const auto native_skipped = g->synthesis_skipped.load();
    std::vector<uint8_t> analysis_bytes;
    const char* analysis_code = "RWTexture2D<float4> O:register(u0);"
        "[numthreads(16,16,1)]void main(uint3 p:SV_DispatchThreadID){O[p.xy]=float4(0,1,0,1);}";
    Com<ID3D11ComputeShader> untagged_analysis, analysis_shader;
    if (!compile(analysis_code,analysis_bytes,untagged_analysis.Put())) return 1;
    g->analysis_shaders.emplace_back(255,analysis_bytes);
    if (FAILED(dev->CreateComputeShader(analysis_bytes.data(),analysis_bytes.size(),nullptr,analysis_shader.Put()))) return 1;
    auto analyze = [&] {
        unbind(); ctx->CSSetShader(analysis_shader.p,nullptr,0);
        ctx->CSSetUnorderedAccessViews(0,1,&outputs[5].p,nullptr);ctx->Dispatch(1,1,1);unbind();
    };
    const auto skipped_before = g->analysis_skipped.load();
    g->test_return_rva = kAnalysisReturns[0]; analyze();
    if (g->analysis_skipped.load() != skipped_before+1 || !pixels(0xff000050)) {
        std::cerr << "exact analysis bypass executed original shader\n"; return 1;
    }
    g->test_return_rva = kAnalysisReturns[0]+1; analyze();
    if (g->analysis_skipped.load() != skipped_before+1 || !pixels(0xff00ff00) || !g->unknown_analysis_call.load()) {
        std::cerr << "unknown call site did not preserve original Dispatch\n"; return 1;
    }
    // Force diagnostic lock contention; independent source-fill fallback must
    // still work after native analysis has been skipped.
    g->test_return_rva=kSourceReturn; source(0xff00008c,2);
    auto* bound = BoundSession(ctx.p); if (!bound) return 1;
    const auto repeats_before=g->repeat_queued.load();
    {
        std::lock_guard<std::mutex> hold(g->backend_mutex);
        t_dispatch=true; unbind();ctx->CSSetShader(synth_shader.p,nullptr,0);
        ID3D11ShaderResourceView* bindings[]{views[1].p,views[2].p};ctx->CSSetShaderResources(0,2,bindings);
        ctx->CSSetUnorderedAccessViews(0,1,&outputs[5].p,nullptr);
        if (!RepeatNative(ctx.p,*bound)) return 1;
        t_dispatch=false; unbind();
    }
    if(g->repeat_queued.load()!=repeats_before+1 || !pixels(0xff00008c) || native_skipped<2) return 1;
    g->requested_mode.store(1); ++g->source_invalidation_epoch;
    g->test_return_rva=kSourceReturn; source(0xff0000a0,1);
    g->test_return_rva=kSynthesisReturn; synthesize(2,1);
    if(!pixels(0xff0000a0)) { std::cerr << "economy to hybrid recovery slot\n";return 1; }
    g->test_return_rva=0; source(0xff0000b4,2); synthesize(1,2);
    if(!pixels(0xffff00ff)) { std::cerr << "hybrid source warmup did not preserve native pixels\n";return 1; }
    std::cout << "Synthetic verified call sites: analysis/synthesis bypass, unknown-site rejection, source fallback and mode recovery passed\n";
    SetNativeProfile(1, 1, 2.0f, false); // Adaptive stays native.
    source(0xff00008c, 2); synthesize(1, 2);
    if (!pixels(0xffff00ff)) { std::cerr << "unsupported profile was replaced\n"; return 1; }
    std::cout << "End-to-end native hooks: original Dispatch, warmup fallback, GPU midpoint replacement and profile gate passed\n";
    // Simulate idle elapsed time directly; the production policy uses the same
    // monotonic clock. No wall-clock sleeps are needed for nine LS restarts.
    SetNativeProfile(1, 0, 2.0f, false);
    const auto reused_before = g->session_reused.load(), limit_before = g->session_limit.load();
    for (uint64_t cycle = 0; cycle != 9; ++cycle) {
        Com<ID3D11Device> restart_device; Com<ID3D11DeviceContext> restart_context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
            restart_device.Put(), nullptr, restart_context.Put()))) return 1;
        Attach(restart_device.p, restart_context.p);
        Com<ID3D11Device5> restart_device5; Com<ID3D11DeviceContext4> restart_context4;
        Com<ID3D11Fence> restart_fence;
        if (FAILED(restart_device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(restart_device5.Put()))) ||
            FAILED(restart_context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(restart_context4.Put()))) ||
            FAILED(restart_device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence), reinterpret_cast<void**>(restart_fence.Put())))) return 1;
        Com<ID3D11ComputeShader> restart_copy, restart_synth;
        if (FAILED(restart_device->CreateComputeShader(g->input_shader.data(), g->input_shader.size(), nullptr, restart_copy.Put())) ||
            FAILED(restart_device->CreateComputeShader(g->shader.data(), g->shader.size(), nullptr, restart_synth.Put()))) return 1;
        std::array<Com<ID3D11Texture2D>, 4> restart_images;
        std::array<Com<ID3D11ShaderResourceView>, 4> restart_views;
        std::array<Com<ID3D11UnorderedAccessView>, 4> restart_outputs;
        for (size_t i = 0; i != restart_images.size(); ++i)
            if (FAILED(restart_device->CreateTexture2D(&td, nullptr, restart_images[i].Put())) ||
                FAILED(restart_device->CreateShaderResourceView(restart_images[i].p, nullptr, restart_views[i].Put())) ||
                FAILED(restart_device->CreateUnorderedAccessView(restart_images[i].p, nullptr, restart_outputs[i].Put()))) return 1;
        Com<ID3D11Buffer> restart_phase;
        if (FAILED(restart_device->CreateBuffer(&desc, &initial, restart_phase.Put()))) return 1;
        restart_context->CSSetConstantBuffers(0, 1, &restart_phase.p);
        const uint64_t now_ms = GetTickCount64();
        if (now_ms < 1000) return 1;
        for (auto& session : g->sessions) if (session.backend) session.last_source_ms = now_ms - 1000;
        uint64_t restart_completion = 0;
        auto restart_unbind = [&] {
            ID3D11ShaderResourceView* nulls[5]{}; ID3D11UnorderedAccessView* null_output = nullptr;
            restart_context->CSSetShaderResources(0, 5, nulls);
            restart_context->CSSetUnorderedAccessViews(0, 1, &null_output, nullptr);
        };
        auto restart_source = [&](uint32_t color, size_t target) {
            restart_unbind(); std::vector<uint32_t> data(256, color);
            restart_context->UpdateSubresource(restart_images[0].p, 0, nullptr, data.data(), 64, 0);
            restart_context->CSSetShader(restart_copy.p, nullptr, 0);
            restart_context->CSSetShaderResources(0, 1, &restart_views[0].p);
            restart_context->CSSetUnorderedAccessViews(0, 1, &restart_outputs[target].p, nullptr);
            restart_context->Dispatch(2, 2, 1); restart_unbind();
        };
        auto restart_synthesize = [&](size_t previous, size_t current) {
            restart_unbind(); restart_context->CSSetShader(restart_synth.p, nullptr, 0);
            ID3D11ShaderResourceView* bindings[]{restart_views[previous].p, restart_views[current].p};
            restart_context->CSSetShaderResources(0, 2, bindings);
            restart_context->CSSetUnorderedAccessViews(0, 1, &restart_outputs[3].p, nullptr);
            restart_context->Dispatch(1, 1, 1); restart_unbind();
        };
        auto restart_drain = [&] {
            if (FAILED(restart_context4->Signal(restart_fence.p, ++restart_completion))) return false;
            restart_context->Flush(); // TEST ONLY
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event) return false;
            const HRESULT hr = restart_fence->SetEventOnCompletion(restart_completion, event);
            const DWORD result = SUCCEEDED(hr) ? WaitForSingleObject(event, 15000) : WAIT_FAILED;
            CloseHandle(event); return result == WAIT_OBJECT_0;
        };
        auto restart_pixels = [&](uint32_t expected) {
            if (!restart_drain()) return false;
            auto read_desc = td; read_desc.BindFlags = 0; read_desc.Usage = D3D11_USAGE_STAGING;
            read_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; Com<ID3D11Texture2D> read;
            if (FAILED(restart_device->CreateTexture2D(&read_desc, nullptr, read.Put()))) return false;
            restart_context->CopyResource(read.p, restart_images[3].p); if (!restart_drain()) return false;
            D3D11_MAPPED_SUBRESOURCE pixels{};
            if (FAILED(restart_context->Map(read.p, 0, D3D11_MAP_READ, 0, &pixels))) return false;
            bool valid = true;
            for (UINT y = 0; y != 16; ++y) for (UINT x = 0; x != 16; ++x) {
                uint32_t pixel = 0; std::memcpy(&pixel, static_cast<const uint8_t*>(pixels.pData) + y * pixels.RowPitch + x * 4, 4);
                valid &= pixel == expected;
            }
            restart_context->Unmap(read.p, 0); return valid;
        };
        restart_source(0xff000014, 1); restart_source(0xff00003c, 2); restart_synthesize(1, 2);
        if (!restart_pixels(0xffff00ff)) { std::cerr << "restart hook warmup\n"; return 1; }
        restart_source(0xff000064, 1); restart_synthesize(2, 1);
        if (!restart_pixels(0xff000050) || g->selected_context.load() != ObjectId(restart_context.p) ||
            g->selection.load() != 1) { std::cerr << "restart hook replacement/routing\n"; return 1; }
    }
    size_t graph_count = 0; for (const auto& session : g->sessions) graph_count += session.backend ? 1 : 0;
    if (graph_count != 1 || g->session_reused.load() != reused_before + 9 || g->session_limit.load() != limit_before) {
        std::cerr << "restart controller retained lifetime-only session limit\n"; return 1;
    }
    std::cout << "Nine real hooked device restarts retain one graph, warm up and replace midpoint pixels without session_limit\n";
    // An Initialize call can hold the controller lock for a long time. The
    // heartbeat must still report that phase instead of producing an empty log.
    std::ostringstream contended;
    std::mutex completed_mutex; std::condition_variable completed_changed;
    bool completed = false;
    std::unique_lock<std::mutex> controller(g->backend_mutex);
    g->backend_phase.store(1);
    std::thread sampler([&] {
        WriteBackendDiagnostics(contended);
        std::lock_guard<std::mutex> lock(completed_mutex); completed = true; completed_changed.notify_all();
    });
    bool sampled_while_busy = false;
    {
        std::unique_lock<std::mutex> lock(completed_mutex);
        sampled_while_busy = completed_changed.wait_for(lock, std::chrono::seconds(5), [&] { return completed; });
    }
    controller.unlock(); sampler.join(); g->backend_phase.store(0);
    if (!sampled_while_busy || contended.str().find("backend_phase=initializing diagnostics_busy=1") == std::string::npos ||
        contended.str().find("observer uptime_ms=") == std::string::npos) {
        std::cerr << "diagnostics lost heartbeat behind controller lock\n"; return 1;
    }
    // An unrelated local utility DLL must remain data: removing the addon
    // manager does not authorize loading arbitrary version.dll replacements.
    wchar_t temporary[32768]{};
    if (!GetTempPathW(32768, temporary)) return 1;
    const auto probe = std::filesystem::path(temporary) / (L"native-sm86-probe-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(probe);
    { std::ofstream unknown(probe / L"version.dll", std::ios::binary); unknown << "unrecognized fixture, never execute"; }
    std::ostringstream runtime;
    PrepareSm86(runtime, probe);
    std::filesystem::remove_all(probe);
    if (runtime.str().find("sm86_bootstrap=no_recognized_utility_proxy") == std::string::npos ||
        runtime.str().find("sm86_bootstrap=loaded") != std::string::npos ||
        runtime.str().find("runtime_file=\"version.dll\" exists=1 sha256=") == std::string::npos) {
        std::cerr << "unknown utility proxy bootstrap was not rejected\n"; return 1;
    }
    // Real MinHook rejection, without modifying a runtime method. Even an
    // observer that has already stopped must persist its failure and footer.
    Hook<DispatchFn> rejected[kHookLimit]{};
    if (Install(rejected, reinterpret_cast<void*>(uintptr_t(1)), HookKind::Dispatch)) return 1;
    StopObservation(StopReason::HookInstall);
    StopObservation(StopReason::TraceComplete); // The first stop cause wins.
    std::ostringstream terminal;
    RunBackendWriter(terminal);
    const auto terminal_text = terminal.str();
    if (terminal_text.find("active=0 stop=hook_install") == std::string::npos ||
        terminal_text.find("hook=Dispatch hook_stage=create") == std::string::npos ||
        terminal_text.find("hook_status=MH_ERROR_NOT_EXECUTABLE") == std::string::npos ||
        terminal_text.find("writer=stopped") == std::string::npos) {
        std::cerr << "terminal observer failure was not persisted\n"; return 1;
    }
    std::cout << "Busy-controller heartbeat and terminal MinHook failure diagnostics passed\n";
#endif
    g->active.store(false);
    std::cout << "WARP creation snapshot, Map invalidation, deferred epoch, source writes, reuse and pair invalidation passed\n";
    return 0;
}
#endif
