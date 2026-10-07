#include "observation_win.h"
#include <cstdint>
#include <mutex>

// ABI reference: Echo-Storm/ls-addon-manager, MIT, main.cpp at
// fbcc3c179e4052785c032d157d8eb785608eb28e. The observer does not link its SDK
// or manager. Ten exports are transparent PE forwarders, including Init/UnInit.
#define LS_SETTINGS_ARGS \
    int scalingMode, int scalingFitMode, int scalingType, int scalingSubtype, \
    float scaleFactor, uint8_t resizeBeforeScale, int sharpness, uint8_t vrs, \
    int frameGenType, int frameGenSize, int frameGenMode, float frameGenMultiplier, \
    float frameGenTarget, int frameGenFlowScale, uint8_t clipCursor, \
    uint8_t adjustCursorSpeed, uint8_t hideCursor, uint8_t scaleCursor, int syncMode, \
    int maxFrameLatency, uint8_t gsyncSupport, uint8_t hdrSupport, int captureApi, \
    int queueTarget, uint8_t drawFps, int gpuId, int displayId, int cropLeft, \
    int cropTop, int cropRight, int cropBottom, uint8_t multiDisplayMode
#define LS_SETTINGS_VALUES \
    scalingMode, scalingFitMode, scalingType, scalingSubtype, scaleFactor, \
    resizeBeforeScale, sharpness, vrs, frameGenType, frameGenSize, frameGenMode, \
    frameGenMultiplier, frameGenTarget, frameGenFlowScale, clipCursor, \
    adjustCursorSpeed, hideCursor, scaleCursor, syncMode, maxFrameLatency, \
    gsyncSupport, hdrSupport, captureApi, queueTarget, drawFps, gpuId, displayId, \
    cropLeft, cropTop, cropRight, cropBottom, multiDisplayMode

extern "C" void __fastcall ApplySettings(LS_SETTINGS_ARGS);
namespace {
HMODULE g_proxy = nullptr;
std::once_flag g_start;
decltype(&ApplySettings) g_apply = nullptr;

void Resolve() {
    wchar_t path[32768]{};
    const DWORD size = GetModuleFileNameW(g_proxy, path, 32768);
    if (!size || size >= 32768) return;
    const auto folder = std::filesystem::path(path).parent_path();
    const auto nativePath = folder / L"Lossless_original.dll";
    const HMODULE native = LoadLibraryExW(nativePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!native) return;
    g_apply = NativeExport<decltype(g_apply)>(native, "ApplySettings");
    if (g_apply) StartNativeObservation(native, g_proxy, folder);
}
}

extern "C" void __fastcall ApplySettings(LS_SETTINGS_ARGS) {
    std::call_once(g_start, Resolve);
    if (!g_apply) {
        // A missing native implementation is an installation failure, not a
        // successful settings update. Normal PE forwarders also cannot run then.
        RaiseException(ERROR_PROC_NOT_FOUND, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        return;
    }
    g_apply(LS_SETTINGS_VALUES); // Every native setting reaches LS unchanged.
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_proxy = module;
        DisableThreadLibraryCalls(module);
    }
    // No D3D, hooks, threads, waits, logging or DLL loading under loader lock.
    return TRUE;
}
