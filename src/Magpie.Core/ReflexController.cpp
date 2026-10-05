#include "pch.h"
#include "ReflexController.h"
#include "Logger.h"

#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
#include "include/NvapiCallGuard.h"
#include <d3d12.h>
#include <nvapi.h>
#include <nvapi_interface.h>
#endif

namespace Magpie {

#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
namespace {

class NvReflexDriver final : public ReflexDriver {
public:
	~NvReflexDriver() override {
		if (_initialized && !_guard.IsFaulted())
			_Call("NvAPI_Unload", [&] { return _unload(); }, NVAPI_ERROR);
		// Do not unload native code after an exception: driver-owned callbacks
		// may still refer to it. Restarting Magpie releases this retained reference.
		if (_guard.IsFaulted()) (void)_module.release();
	}
	bool Initialize(ID3D11Device* device) noexcept {
		_device.copy_from(device);
		const auto dxgiDevice = _device.try_as<IDXGIDevice>();
		winrt::com_ptr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC adapterDesc{};
		if (!dxgiDevice || FAILED(dxgiDevice->GetAdapter(adapter.put())) ||
			FAILED(adapter->GetDesc(&adapterDesc))) {
			ReportFailure("query presentation adapter", NVAPI_INVALID_ARGUMENT);
			return false;
		}
		_adapterLuid = adapterDesc.AdapterLuid;
		// The driver is supplied by Windows/NVIDIA, never by an effect directory.
		_module.reset(LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
		if (!_module) {
			ReportFailure("load system nvapi64.dll", static_cast<int>(GetLastError()));
			return false;
		}
		_query = reinterpret_cast<QueryInterface>(GetProcAddress(_module.get(), "nvapi_QueryInterface"));
		if (!_query || !_Resolve(_initialize, "NvAPI_Initialize") ||
			!_Resolve(_unload, "NvAPI_Unload") ||
			!_Resolve(_setSleepMode, "NvAPI_D3D_SetSleepMode") ||
			!_Resolve(_getSleepStatus, "NvAPI_D3D_GetSleepStatus") ||
			!_Resolve(_sleep, "NvAPI_D3D_Sleep") ||
			!_Resolve(_marker, "NvAPI_D3D_SetLatencyMarker")) {
			ReportFailure("resolve native Reflex pacing interfaces", NVAPI_NO_IMPLEMENTATION);
			return false;
		}
		// Availability of optional async tracking must not govern ordinary Reflex.
		_Resolve(_async11, "NvAPI_D3D11_SetAsyncFrameMarker");
		_Resolve(_async12, "NvAPI_D3D12_SetAsyncFrameMarker");
		_Resolve(_outOfBand, "NvAPI_D3D12_NotifyOutOfBandCommandQueue");
		const NvAPI_Status status = _Call("NvAPI_Initialize", [&] { return _initialize(); }, NVAPI_ERROR);
		_initialized = status == NVAPI_OK;
		if (!_initialized) ReportFailure("NvAPI_Initialize", status);
		return _initialized;
	}
	ReflexConfigurationResult Configure(ReflexSettings settings) noexcept override {
		NV_SET_SLEEP_MODE_PARAMS options{};
		options.version = NV_SET_SLEEP_MODE_PARAMS_VER;
		options.bLowLatencyMode = settings.lowLatency;
		options.bLowLatencyBoost = settings.lowLatency && settings.boost;
		options.minimumIntervalUs = settings.minimumIntervalUs;
		// Marker-based CPU optimization needs separately validated Boost timing.
		// Standard markers remain active without enabling this extra optimization.
		ReflexConfigurationResult result;
		result.setStatus = _Call("SetSleepMode", [&] { return _setSleepMode(_device.get(), &options); }, NVAPI_ERROR);
		NV_GET_SLEEP_STATUS_PARAMS state{};
		state.version = NV_GET_SLEEP_STATUS_PARAMS_VER;
		if (result.setStatus == NVAPI_OK) {
			result.queried = true;
			result.queryStatus = _Call("GetSleepStatus", [&] { return _getSleepStatus(_device.get(), &state); }, NVAPI_ERROR);
			result.lowLatency = result.queryStatus == NVAPI_OK && state.bLowLatencyMode;
		}
		Logger::Get().Info(fmt::format(
			"Reflex configuration: requestedOn={} Boost={} minimumIntervalUs={} "
			"SetSleepMode={} queried={} GetSleepStatus={} actualOn={}; capture-to-present scope",
			settings.lowLatency, settings.boost, settings.minimumIntervalUs, result.setStatus,
			result.queried, result.queryStatus, result.lowLatency));
		return result;
	}
	int Sleep() noexcept override {
		return _Call("Sleep", [&] { return _sleep(_device.get()); }, NVAPI_ERROR);
	}
	int Marker(ReflexMarker marker, uint64_t frameId) noexcept override {
		NV_LATENCY_MARKER_PARAMS params{};
		params.version = NV_LATENCY_MARKER_PARAMS_VER;
		params.frameID = frameId;
		switch (marker) {
		case ReflexMarker::SimulationStart: params.markerType = SIMULATION_START; break;
		case ReflexMarker::SimulationEnd: params.markerType = SIMULATION_END; break;
		case ReflexMarker::RenderStart: params.markerType = RENDERSUBMIT_START; break;
		case ReflexMarker::RenderEnd: params.markerType = RENDERSUBMIT_END; break;
		}
		return _Call("SetLatencyMarker", [&] { return _marker(_device.get(), &params); }, NVAPI_ERROR);
	}
	int RegisterGenerationQueue(ID3D12CommandQueue* queue) noexcept override {
		if (!_outOfBand) return NVAPI_NO_IMPLEMENTATION;
		winrt::com_ptr<ID3D12Device> device;
		if (!queue || FAILED(queue->GetDevice(IID_PPV_ARGS(device.put())))) return NVAPI_INVALID_ARGUMENT;
		const LUID luid = device->GetAdapterLuid();
		if (luid.HighPart != _adapterLuid.HighPart || luid.LowPart != _adapterLuid.LowPart)
			return NVAPI_INVALID_COMBINATION;
		return _Call("NotifyOutOfBandCommandQueue", [&] { return _outOfBand(queue, OUT_OF_BAND_RENDER); }, NVAPI_ERROR);
	}
	int Generation(ID3D12CommandQueue* queue, uint64_t frameId,
		uint64_t presentId, bool start) noexcept override {
		if (!_async12) return NVAPI_NO_IMPLEMENTATION;
		auto params = _AsyncParams(frameId, presentId,
			start ? OUT_OF_BAND_RENDERSUBMIT_START : OUT_OF_BAND_RENDERSUBMIT_END);
		return _Call("D3D12 async generation marker", [&] { return _async12(queue, &params); }, NVAPI_ERROR);
	}
	int FrontendRender(uint64_t frameId, uint64_t presentId, bool start) noexcept override {
		if (!_async11) return NVAPI_NO_IMPLEMENTATION;
		auto params = _AsyncParams(frameId, presentId,
			start ? OUT_OF_BAND_RENDERSUBMIT_START : OUT_OF_BAND_RENDERSUBMIT_END);
		return _Call("D3D11 async render marker", [&] { return _async11(_device.get(), &params); }, NVAPI_ERROR);
	}
	int Present(uint64_t frameId, uint64_t presentId, bool generated, bool start) noexcept override {
		if (!_async11) return NVAPI_NO_IMPLEMENTATION;
		auto params = _AsyncParams(frameId, presentId, generated
			? (start ? OUT_OF_BAND_PRESENT_START : OUT_OF_BAND_PRESENT_END)
			: (start ? PRESENT_START : PRESENT_END));
		return _Call("D3D11 async present marker", [&] { return _async11(_device.get(), &params); }, NVAPI_ERROR);
	}
	void ReportFailure(const char* operation, int status) noexcept override {
		Logger::Get().Warn(fmt::format("Reflex call failed: operation={} status={}", operation, status));
	}

private:
	template<typename Function, typename Result>
	Result _Call(const char* operation, Function&& function, Result failure) noexcept {
		DWORD sehCode = 0;
		const auto result = _guard.Invoke(function, failure, &sehCode);
		if (sehCode && !_faultReported.exchange(true)) {
			Logger::Get().Warn(fmt::format(
				"Reflex native exception: operation={} code=0x{:08x} address=0x{:x} thread={}; "
				"native calls disabled for this session; restart Magpie before retrying Reflex",
				operation, sehCode, _guard.FaultAddress(), _guard.FaultThread()));
		}
		return result;
	}
	static NV_ASYNC_FRAME_MARKER_PARAMS _AsyncParams(uint64_t frameId,
		uint64_t presentId, NV_LATENCY_MARKER_TYPE marker) noexcept {
		NV_ASYNC_FRAME_MARKER_PARAMS params{};
		params.version = NV_ASYNC_FRAME_MARKER_PARAMS_VER;
		params.frameID = frameId;
		params.presentFrameID = presentId;
		params.markerType = marker;
		// vendorInternal and all reserved fields remain zero.
		return params;
	}
	template<class T> bool _Resolve(T& function, const char* name) noexcept {
		for (const auto& entry : nvapi_interface_table) {
			if (std::strcmp(entry.func, name) == 0) {
				function = reinterpret_cast<T>(_Call("nvapi_QueryInterface",
					[&] { return _query(entry.id); }, static_cast<void*>(nullptr)));
				return function != nullptr;
			}
		}
		return false;
	}
	using QueryInterface = void* (__cdecl*)(unsigned int);
	wil::unique_hmodule _module;
	winrt::com_ptr<ID3D11Device> _device;
	LUID _adapterLuid{};
	QueryInterface _query = nullptr;
	decltype(&NvAPI_Initialize) _initialize = nullptr;
	decltype(&NvAPI_Unload) _unload = nullptr;
	decltype(&NvAPI_D3D_SetSleepMode) _setSleepMode = nullptr;
	decltype(&NvAPI_D3D_GetSleepStatus) _getSleepStatus = nullptr;
	decltype(&NvAPI_D3D_Sleep) _sleep = nullptr;
	decltype(&NvAPI_D3D_SetLatencyMarker) _marker = nullptr;
	decltype(&NvAPI_D3D11_SetAsyncFrameMarker) _async11 = nullptr;
	decltype(&NvAPI_D3D12_SetAsyncFrameMarker) _async12 = nullptr;
	decltype(&NvAPI_D3D12_NotifyOutOfBandCommandQueue) _outOfBand = nullptr;
	bool _initialized = false;
	NvapiCallGuard _guard;
	std::atomic<bool> _faultReported = false;
};

}
#endif

std::unique_ptr<ReflexDriver> CreateNvReflexDriver(ID3D11Device* presentDevice) noexcept {
#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
	if (!presentDevice) return nullptr;
	auto driver = std::make_unique<NvReflexDriver>();
	if (!driver->Initialize(presentDevice)) return nullptr;
	return driver;
#else
	(void)presentDevice;
	return nullptr;
#endif
}

}
