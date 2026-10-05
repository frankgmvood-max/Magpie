#pragma once
#include "Logger.h"
#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
#include <nvapi.h>
#include <nvapi_interface.h>
#endif

namespace Magpie {

// Read only, once after warm-up. Flags are driver state, not a measurement of
// panel scanout intervals. No DRS/profile writes or per-frame NVAPI polling.
inline void LogVrrDriverState(ID3D11Device* device, IDXGISwapChain* chain, const RECT& outputRect) noexcept {
#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
	winrt::com_ptr<IDXGIDevice> dxgi;
	winrt::com_ptr<IDXGIAdapter> adapter;
	DXGI_ADAPTER_DESC desc{};
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(dxgi.put()))) ||
		FAILED(dxgi->GetAdapter(adapter.put())) || FAILED(adapter->GetDesc(&desc)) ||
		desc.VendorId != 0x10de) return;
	wil::unique_hmodule module(LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
	if (!module) return;
	const auto query = reinterpret_cast<void* (__cdecl*)(unsigned int)>(GetProcAddress(module.get(), "nvapi_QueryInterface"));
	if (!query) return;
	auto resolve = [query](const char* name) -> void* {
		for (const auto& entry : nvapi_interface_table)
			if (std::strcmp(entry.func, name) == 0) return query(entry.id);
		return nullptr;
	};
	const auto initialize = reinterpret_cast<decltype(&NvAPI_Initialize)>(resolve("NvAPI_Initialize"));
	const auto unload = reinterpret_cast<decltype(&NvAPI_Unload)>(resolve("NvAPI_Unload"));
	if (!initialize || !unload || initialize() != NVAPI_OK) return;
	const auto cleanup = wil::scope_exit([unload] { unload(); });
	const auto isActive = reinterpret_cast<decltype(&NvAPI_D3D_IsGSyncActive)>(resolve("NvAPI_D3D_IsGSyncActive"));
	BOOL active = FALSE;
	const int activeStatus = isActive ? isActive(device, reinterpret_cast<NVDX_ObjectHandle>(chain), &active) : NVAPI_NO_IMPLEMENTATION;
	Logger::Get().Info(fmt::format("VRR driver state: GSyncActive={} status={} foreground=0x{:x}; indicator is not physical Hz telemetry",
		active != FALSE, activeStatus, reinterpret_cast<uintptr_t>(GetForegroundWindow())));
#ifdef NV_GET_VRR_INFO_VER
	MONITORINFOEXA monitor{}; monitor.cbSize = sizeof(monitor);
	if (!GetMonitorInfoA(MonitorFromRect(&outputRect, MONITOR_DEFAULTTONEAREST), &monitor)) return;
	const auto displayId = reinterpret_cast<decltype(&NvAPI_DISP_GetDisplayIdByDisplayName)>(resolve("NvAPI_DISP_GetDisplayIdByDisplayName"));
	const auto vrrInfo = reinterpret_cast<decltype(&NvAPI_Disp_GetVRRInfo)>(resolve("NvAPI_Disp_GetVRRInfo"));
	NvU32 id = 0;
	if (displayId && vrrInfo && displayId(monitor.szDevice, &id) == NVAPI_OK) {
		NV_GET_VRR_INFO info{}; info.version = NV_GET_VRR_INFO_VER;
		const int status = vrrInfo(id, &info);
		Logger::Get().Info(fmt::format("VRR display state: display={} status={} possible={} enabled={} requested={} displayInVrrMode={}; read-only driver report, no measured scanout Hz",
			monitor.szDevice, status, bool(info.bIsVRRPossible), bool(info.bIsVRREnabled), bool(info.bIsVRRRequested), bool(info.bIsDisplayInVRRMode)));
	}
#endif
#else
	(void)device; (void)chain; (void)outputRect;
#endif
}

}
