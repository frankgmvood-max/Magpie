#include "observation_win.h"
#include "../include/observation.h"
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
#endif

namespace {
using namespace ls_native;
constexpr char kDllHash[] = "626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb";
constexpr char kShaderHash[] = "3af0031e97f43a9372c23749ebbbe91506119268d84e869ba715ffe71bb9d45a";
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
struct ConstantTag { ConstantBytes bytes; uint64_t command_epoch = 0; uint32_t usage = 0; };
template<class Fn> struct Hook { void* target = nullptr; Fn original = nullptr; };
constexpr int kHookLimit = 12;
template<class Fn> bool Install(Hook<Fn>(&hooks)[kHookLimit], void* target);

// Pinned, process-lifetime observer. Hooks and trampolines are never freed while
// a native thread could be returning through them. No shutdown work in DllMain.
struct State {
    std::atomic<bool> active{false};
    std::atomic<uint64_t> serial{0}, seen{0}, untagged{0}, busy{0}, tagged_shaders{0}, tagged_buffers{0}, callbacks{0};
    std::mutex capture_mutex, install_mutex;
    ObservationQueue<DispatchObservation, 512> records;
    std::vector<uint8_t> shader;
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
    std::atomic<uint64_t> command_epoch{0};
    CreateDeviceFn create = nullptr;
    uint32_t duration = 20, max_records = 4096;
    uint64_t frequency = 0;
};
State* g = nullptr;
thread_local bool t_dispatch = false, t_shader = false, t_buffer = false;
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

void Observe(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    // This is deliberately metadata-only. No Map, GPU copy, fence query/wait,
    // Flush, Present, shader replacement or settings adjustment.
    Com<ID3D11ComputeShader> shader;
    ctx->CSGetShader(shader.Put(), nullptr, nullptr);
    uint32_t resource_id = 0;
    if (!ReadTag(shader.p, kShaderTag, resource_id) || resource_id != 256) return;
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) { ++g->untagged; return; }
    const uint64_t sequence = ++g->seen;
    if (sequence > g->max_records) { g->active.store(false); return; }
    std::unique_lock<std::mutex> lock(g->capture_mutex, std::try_to_lock);
    if (!lock.owns_lock()) { ++g->busy; return; }
    Com<ID3D11Device> device;
    ctx->GetDevice(device.Put());
    DeviceTag tag;
    if (!ReadTag(device.p, kDeviceTag, tag)) { ++g->untagged; return; }
    DispatchObservation record;
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

    for (UINT i = 0; i != 5; ++i) {
        Com<ID3D11ShaderResourceView> view;
        ctx->CSGetShaderResources(i, 1, view.Put());
        if (!view.p) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; view->GetDesc(&desc);
        if (desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) continue;
        Com<ID3D11Resource> texture; view->GetResource(texture.Put());
        Texture(texture.p, desc.Format, desc.Texture2D.MostDetailedMip, record.inputs[i]);
    }
    Com<ID3D11UnorderedAccessView> output;
    ctx->CSGetUnorderedAccessViews(0, 1, output.Put());
    if (output.p) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC desc{}; output->GetDesc(&desc);
        if (desc.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D) {
            Com<ID3D11Resource> texture; output->GetResource(texture.Put());
            Texture(texture.p, desc.Format, desc.Texture2D.MipSlice, record.output);
        }
    }
    g->records.TryPush(record);
}

template<int I> HRESULT STDMETHODCALLTYPE Shader(ID3D11Device* dev, const void* bytes, SIZE_T size,
    ID3D11ClassLinkage* linkage, ID3D11ComputeShader** result) {
    const auto original = g->shaders[I].original;
    if (t_shader || !g->active.load()) return original(dev, bytes, size, linkage, result);
    t_shader = true;
    const HRESULT hr = original(dev, bytes, size, linkage, result);
    DeviceTag device;
    if (SUCCEEDED(hr) && result && *result && bytes && ReadTag(dev, kDeviceTag, device) &&
        size == g->shader.size() && std::memcmp(bytes, g->shader.data(), size) == 0) {
        const uint32_t id = 256;
        if (SUCCEEDED((*result)->SetPrivateData(kShaderTag, sizeof(id), &id))) ++g->tagged_shaders;
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
template<int I> void STDMETHODCALLTYPE Dispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    const auto original = g->dispatches[I].original;
    if (t_dispatch) { original(ctx, x, y, z); return; }
    ++g->callbacks;
    if (g->active.load()) {
        t_dispatch = true;
        try { Observe(ctx, x, y, z); } catch (...) { ++g->untagged; }
        original(ctx, x, y, z); // Exactly once. Native output is always produced.
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
    else return {reinterpret_cast<void*>(&Execute<I>)...};
}

bool InstallContext(ID3D11DeviceContext* ctx) {
    void** table = *reinterpret_cast<void***>(ctx);
    bool complete = Install(g->dispatches, table[41]);
    complete &= Install(g->maps, table[14]);
    complete &= Install(g->copies, table[47]);
    complete &= Install(g->regions, table[46]);
    complete &= Install(g->updates, table[48]);
    complete &= Install(g->executions, table[58]);
    Com<ID3D11DeviceContext1> ctx1;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(ctx1.Put())))) {
        void** extended = *reinterpret_cast<void***>(ctx1.p);
        complete &= Install(g->regions1, extended[115]);
        complete &= Install(g->updates1, extended[116]);
    }
    return complete;
}
template<class Fn> bool Install(Hook<Fn>(&hooks)[kHookLimit], void* target) {
    for (const auto& hook : hooks) if (hook.target == target) return true;
    for (int i = 0; i != kHookLimit; ++i) {
        if (hooks[i].target) continue;
        const auto detours = Detours<Fn>(std::make_integer_sequence<int,kHookLimit>{});
        void* trampoline = nullptr;
        if (MH_CreateHook(target, detours[i], &trampoline) != MH_OK) return false;
        static_assert(sizeof(trampoline) == sizeof(hooks[i].original), "Windows function pointer ABI");
        std::memcpy(&hooks[i].original, &trampoline, sizeof(trampoline));
        hooks[i].target = target; // Published before any native thread can enter.
        if (MH_EnableHook(target) == MH_OK) return true;
        MH_RemoveHook(target); hooks[i] = {};
        return false;
    }
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
    if (FAILED(dev->SetPrivateData(kDeviceTag, sizeof(tag), &tag))) return;
    void** table = *reinterpret_cast<void***>(dev);
    const bool context_complete = InstallContext(ctx);
    const bool buffer_complete = Install(g->buffers, table[3]);
    const bool shader_complete = Install(g->shaders, table[18]);
    if (!context_complete || !buffer_complete || !shader_complete) {
        ++g->untagged;
        g->active.store(false); // Incomplete write observation cannot certify phases.
    }
}
HRESULT WINAPI CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* levels, UINT count, UINT sdk, ID3D11Device** dev,
    D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** ctx) {
    const HRESULT hr = g->create(adapter, type, software, flags, levels, count, sdk, dev, level, ctx);
    try {
        if (g->active.load() && SUCCEEDED(hr) && dev && *dev) {
            if (ctx && *ctx) Attach(*dev, *ctx);
            else { Com<ID3D11DeviceContext> immediate; (*dev)->GetImmediateContext(immediate.Put()); Attach(*dev, immediate.p); }
        }
    } catch (...) { ++g->untagged; }
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
void WriteRecord(const DispatchObservation& r) {
    auto& out = g->log;
    out << "{\"kind\":\"dispatch\",\"sequence\":" << r.sequence << ",\"qpc\":" << r.qpc
        << ",\"device\":" << r.device << ",\"context\":" << r.context << ",\"adapter_luid\":" << r.adapter_luid
        << ",\"thread\":" << r.thread << ",\"groups\":[" << r.groups_x << ',' << r.groups_y << ',' << r.groups_z
        << "],\"constants_known\":" << (r.constants_known ? "true" : "false") << ",\"phase_bits\":" << r.phase_bits
        << ",\"resolution_scale_bits\":" << r.resolution_scale_bits << ",\"cb_byte_width\":" << r.cb_byte_width
        << ",\"first_constant\":" << r.first_constant << ",\"num_constants\":" << r.num_constants << ",\"inputs\":[";
    for (size_t i = 0; i != r.inputs.size(); ++i) { if (i) out << ','; WriteTexture(out, r.inputs[i]); }
    out << "],\"output\":"; WriteTexture(out, r.output); out << "}\n";
}
void Writer() {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(g->duration);
    uint64_t written = 0;
    while (g->active.load() && std::chrono::steady_clock::now() < end && g->log) {
        DispatchObservation record;
        while (g->records.TryPop(record)) { WriteRecord(record); ++written; }
        g->log.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Worker only.
    }
    g->active.store(false);
    while (g->callbacks.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
        if (GetPrivateProfileIntW(L"Observation", L"Enabled", 0, ini.c_str()) != 1) return;
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
        // Pin before installing executable detours. Never remove a trampoline
        // while native callers might still be inside it or unload this module.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(proxy), &pinned)) return;
        g = new State; // Deliberate process-lifetime storage; no loader-lock teardown.
        g->shader.assign(bytes, bytes + size);
        g->duration = std::clamp(GetPrivateProfileIntW(L"Observation", L"DurationSeconds", 20, ini.c_str()), 1u, 120u);
        g->max_records = std::clamp(GetPrivateProfileIntW(L"Observation", L"MaxRecords", 4096, ini.c_str()), 1u, 65536u);
        LARGE_INTEGER frequency{}, start{};
        QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start);
        g->frequency = static_cast<uint64_t>(frequency.QuadPart);
        std::filesystem::create_directories(folder / L"logs");
        g->log.open(folder / L"logs" / (L"native-observation-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(start.QuadPart) + L".jsonl"));
        if (!g->log) return;
        g->log << "{\"kind\":\"header\",\"schema_version\":1,\"dll_sha256\":\"" << kDllHash
            << "\",\"shader_sha256\":\"" << kShaderHash << "\",\"resource_id\":256,\"qpc_frequency\":" << g->frequency
            << ",\"present_owner\":\"LS\",\"replacement_enabled\":false,\"source_frame_ids_known\":false}\n";
        g->log.flush();
        const auto d3d = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        g->create = d3d ? NativeExport<CreateDeviceFn>(d3d, "D3D11CreateDevice") : nullptr;
        void** const slot = DelayDeviceSlot(native);
        if (!g->create || !slot || MH_Initialize() != MH_OK) return;
        // Resolve the runtime's normal and multithread-refresh Dispatch entries
        // on a WARP probe; no native LS bindings are changed for probing.
        for (const UINT flags : {0u, UINT(D3D11_CREATE_DEVICE_SINGLETHREADED)}) {
            Com<ID3D11Device> probe; Com<ID3D11DeviceContext> ctx;
            if (FAILED(g->create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                D3D11_SDK_VERSION, probe.Put(), nullptr, ctx.Put()))) continue;
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
        // Start the writer before enabling observations. Constructor failure
        // leaves active=false; every installed detour still forwards unchanged.
        g->active.store(true);
        std::thread writer(Writer);
        if (!PatchSlot(slot, reinterpret_cast<void*>(&CreateDevice))) g->active.store(false);
        writer.detach();
    } catch (...) {
        if (g) g->active.store(false);
        OutputDebugStringW(L"Native DLSS observer failed; leaving LS native.\n");
    }
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
    g->active.store(false);
    std::cout << "WARP creation snapshot, Map invalidation and deferred epoch passed\n";
    return 0;
}
#endif
