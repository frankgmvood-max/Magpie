#include "observation_win.h"
#include "../include/observation.h"
#include "../include/source_pair.h"
#ifdef LS_WITH_NGX
#include "../backend/native_backend.h"
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
    struct Session {
        uint64_t context = 0;
        TextureObservation extent;
        std::unique_ptr<NativeBackend> backend;
    };
    BackendOptions options;
    std::filesystem::path folder;
    std::mutex backend_mutex;
    std::array<Session, 4> sessions;
    std::ofstream backend_log;
    std::atomic<uint64_t> source_calls{0}, slots_seen{0}, profile_revision{0}, backend_busy{0}, session_limit{0};
    std::atomic<int> profile_type{0}, profile_mode{0}, profile_hdr{0};
    std::atomic<float> profile_multiplier{0};
    std::atomic<uint32_t> backend_phase{0}; // 0 idle, 1 Initialize, 2 Source, 3 Composite.
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
void BackendSource(ID3D11DeviceContext* ctx, ID3D11Resource* source, const SourceWriteObservation& write) {
    if (!BackendEnabled() || !SourceWriteValid(write)) return;
    ++g->source_calls;
    std::unique_lock<std::mutex> lock(g->backend_mutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++g->backend_busy; ++g->source_invalidation_epoch; return; }
    BackendActivity activity;
    g->backend_phase.store(2);
    Com<ID3D11Texture2D> texture;
    if (!source || FAILED(source->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(texture.Put())))) return;
    auto* session = FindSession(write.context, write.texture);
    if (!session) for (auto& candidate : g->sessions) {
        if (candidate.backend) continue;
        candidate.context = write.context; candidate.extent = write.texture;
        candidate.backend = std::make_unique<NativeBackend>();
        // One startup allocation on the native owning thread. No LS device is
        // touched by the logger, including SINGLETHREADED devices.
        g->backend_phase.store(1);
        candidate.backend->Initialize(ctx, write.texture, g->options, g->folder);
        g->backend_phase.store(2);
        session = &candidate; break;
    }
    if (session) session->backend->Source(texture.p, write);
    else ++g->session_limit;
}
void BackendComposite(ID3D11DeviceContext* ctx, const DispatchObservation& record) {
    if (!BackendEnabled() || !record.pair_matches_observed_updates) return;
    ++g->slots_seen;
    std::unique_lock<std::mutex> lock(g->backend_mutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++g->backend_busy; return; }
    BackendActivity activity;
    g->backend_phase.store(3);
    auto* session = FindSession(record.context, record.current_write.texture);
    Com<ID3D11UnorderedAccessView> destination; ctx->CSGetUnorderedAccessViews(0, 1, destination.Put());
    if (session) session->backend->Composite(record, destination.p);
}
void WriteBackendDiagnostics(std::ostream& out) {
    struct SessionSnapshot {
        uint64_t context = 0;
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
        << " backend_phase=" << BackendPhaseName(g->backend_phase.load())
        << " diagnostics_busy=" << !controller_sampled << "\n";
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
        out << " step=" << d.step << " code=0x" << std::hex << d.code << std::dec
            << " analysis=" << d.analysis_width << 'x' << d.analysis_height
            << " analysis_format=" << d.analysis_format << " flow_grid=" << d.grid
            << " submitted=" << c.submitted << " composite_queued=" << c.composite_queued
            << " busy=" << c.busy << " unmatched=" << c.mismatched << " flag_enabled_samples=" << c.gpu_enabled
            << " flag_disabled_samples=" << c.gpu_disabled << " failures=" << c.failed
            << " ineligible=" << c.ineligible << " destination_rejected=" << c.destination_rejected << " warmup=" << c.warmup << "\n";
    }
    out.flush();
}
void RunBackendWriter(std::ostream& out) {
    for (;;) {
        WriteBackendDiagnostics(out);
        if (!g->active.load()) {
            out << "writer=stopped\n"; out.flush();
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
void BackendWriter() { RunBackendWriter(g->backend_log); }
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
    if (t_dispatch) { original(ctx, x, y, z); return; }
    ++g->callbacks; ++g->dispatch_calls;
    if (g->active.load()) {
        t_dispatch = true;
        DispatchObservation record;
        try { InvalidateComputeOutputs(ctx); Observe(ctx, x, y, z, false, &record); }
        catch (...) { ++g->untagged; ++g->source_invalidation_epoch; }
        original(ctx, x, y, z); // Exactly once. Native output is always produced.
        try {
            if (g->active.load()) {
#ifdef LS_WITH_NGX
                BackendComposite(ctx, record);
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
        g->options.enabled = backend; g->folder = folder;
        g->options.optical_flow = GetPrivateProfileIntW(L"NativeDLSS", L"OpticalFlow", 1, ini.c_str()) == 1;
        g->options.gpu_ordered = GetPrivateProfileIntW(L"NativeDLSS", L"GPUOrdered", 1, ini.c_str()) == 1;
        g->options.quality = std::clamp(GetPrivateProfileIntW(L"NativeDLSS", L"Quality", 2, ini.c_str()), 1u, 5u);
        g->options.analysis_percent = std::clamp(GetPrivateProfileIntW(L"NativeDLSS", L"AnalysisPercent", 50, ini.c_str()), 10u, 100u);
        g->options.slots = std::clamp(GetPrivateProfileIntW(L"NativeDLSS", L"Slots", 3, ini.c_str()), 2u, 4u);
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
            g->backend_log << "NativeDLSS 0.1.2 experimental. Present owner: LS. Full source coverage / real RTX3080 acceptance: unverified.\n"
                << "OpticalFlow=" << g->options.optical_flow << " Quality=" << g->options.quality
                << " AnalysisPercent=" << g->options.analysis_percent << " GPUOrdered=" << g->options.gpu_ordered
                << " Slots=" << g->options.slots << "\n";
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
    SetNativeProfile(1, 1, 2.0f, false); // Adaptive stays native.
    source(0xff00008c, 2); synthesize(1, 2);
    if (!pixels(0xffff00ff)) { std::cerr << "unsupported profile was replaced\n"; return 1; }
    std::cout << "End-to-end native hooks: original Dispatch, warmup fallback, GPU midpoint replacement and profile gate passed\n";
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
