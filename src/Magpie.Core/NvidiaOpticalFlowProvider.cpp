#include "pch.h"
#include "FrameTrace.h"
#include "NvidiaOpticalFlowProvider.h"
#include "NativeBackendTiming.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "OpticalFlowResolution.h"
#include "OpticalFlowHistory.h"
#include "NvidiaOpticalFlowShaders.h"

#ifdef MP_ENABLE_NVIDIA_OPTICAL_FLOW
#include <d3dcompiler.h>
#include <nvOpticalFlowD3D11.h>

namespace Magpie {

namespace {


template <typename T>
T GetExport(HMODULE module, const char* name) noexcept {
	return reinterpret_cast<T>(GetProcAddress(module, name));
}

bool HasFormat(const std::vector<DXGI_FORMAT>& formats, DXGI_FORMAT value) {
	return std::find(formats.begin(), formats.end(), value) != formats.end();
}

constexpr std::array<float, 2> DecodeS105(int16_t x, int16_t y) noexcept {
	return { float(x) / 32.0f, float(y) / 32.0f };
}

// Compile-time convention probes for the two axis cases used by the manual
// translation test. NVOF stores five fractional bits and we preserve X/Y signs.
static_assert(DecodeS105(64, 0)[0] == 2.0f);
static_assert(DecodeS105(0, -96)[1] == -3.0f);

struct NvofProfile {
	uint32_t gridSize;
	NV_OF_PERF_LEVEL perfLevel;
	std::string_view label;
	std::string_view perfLabel;
};

constexpr NvofProfile ResolveProfile(
	NvidiaOpticalFlowQuality quality
) noexcept {
	switch (quality) {
	case NvidiaOpticalFlowQuality::Performance:
		return { 4, NV_OF_PERF_LEVEL_FAST, "4F", "FAST" };
	case NvidiaOpticalFlowQuality::Quality:
		return { 4, NV_OF_PERF_LEVEL_SLOW, "4S", "SLOW" };
	case NvidiaOpticalFlowQuality::HighQuality:
		return { 2, NV_OF_PERF_LEVEL_MEDIUM, "2M", "MEDIUM" };
	case NvidiaOpticalFlowQuality::HighestQuality:
		return { 2, NV_OF_PERF_LEVEL_SLOW, "2S", "SLOW" };
	case NvidiaOpticalFlowQuality::Balanced:
	default:
		return { 4, NV_OF_PERF_LEVEL_MEDIUM, "4M", "MEDIUM" };
	}
}

FrameGuidanceMetadata MakeMetadata(
	const FrameGuidanceFrame& frame,
	FrameGuidanceResetReason resetReason,
	bool isZero
) noexcept {
	return {
		.frameId = frame.frameId,
		.captureSequence = frame.captureSequence,
		.resourceGeneration = frame.resourceGeneration,
		.timestamp100ns = frame.timestamp100ns,
		.sourceExtent = frame.sourceExtent,
		.validRegion = frame.validRegion,
		.resetReason = resetReason,
		.valid = true,
		.isZero = isZero,
		.requiresHistoryReset = resetReason != FrameGuidanceResetReason::None
	};
}

struct NvofTimingSummary {
	double average = 0.0;
	double p95 = 0.0;
	double p99 = 0.0;
	double maximum = 0.0;
	size_t count = 0;
};

struct NvofTimingWindow {
	static constexpr size_t CAPACITY = 120;
	std::array<double, CAPACITY> values{};
	size_t count = 0;
	size_t next = 0;

	void Add(double value) noexcept {
		values[next] = value;
		next = (next + 1) % CAPACITY;
		count = std::min(count + 1, CAPACITY);
	}

	NvofTimingSummary Summarize() const noexcept {
		NvofTimingSummary result{ .count = count };
		if (!count) return result;
		std::array<double, CAPACITY> sorted{};
		std::copy_n(values.begin(), count, sorted.begin());
		std::sort(sorted.begin(), sorted.begin() + count);
		double total = 0.0;
		for (size_t i = 0; i < count; ++i) total += sorted[i];
		result.average = total / static_cast<double>(count);
		result.p95 = sorted[std::min(count - 1, (count * 95 + 99) / 100 - 1)];
		result.p99 = sorted[std::min(count - 1, (count * 99 + 99) / 100 - 1)];
		result.maximum = sorted[count - 1];
		return result;
	}
};

}

struct NvidiaOpticalFlowProvider::Impl {
	using CreateApiFn = NV_OF_STATUS(NVOFAPI*)(
		uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
	using GetMaxVersionFn = NV_OF_STATUS(NVOFAPI*)(uint32_t*);
	static constexpr uint32_t GPU_QUERY_SLOT_COUNT = 4;
	struct GpuQuerySlot {
		winrt::com_ptr<ID3D11Query> disjoint;
		winrt::com_ptr<ID3D11Query> start;
		winrt::com_ptr<ID3D11Query> end;
		bool pending = false;
	};

	~Impl() { DestroySession(); }

	void DestroySession() noexcept {
		if (session && api.nvOFUnregisterResourceD3D11) {
			for (NvOFGPUBufferHandle& handle : registered) {
				if (handle) {
					api.nvOFUnregisterResourceD3D11(handle);
					handle = nullptr;
				}
			}
		}
		for (NvOFGPUBufferHandle& handle : registered) handle = nullptr;
		if (session && api.nvOFDestroy) {
			api.nvOFDestroy(session);
		}
		session = nullptr;
		api = {};
		if (module) {
			FreeLibrary(module);
			module = nullptr;
		}

		for (auto& texture : input) texture = nullptr;
		for (auto& rtv : inputRtv) rtv = nullptr;
		for (auto& texture : flow) texture = nullptr;
		for (auto& texture : cost) texture = nullptr;
		for (auto& srv : flowSrv) srv = nullptr;
		for (auto& srv : costSrv) srv = nullptr;
		motion = nullptr;
		confidence = nullptr;
		motionUav = nullptr;
		confidenceUav = nullptr;
		densifyShader = nullptr;
		paramsBuffer = nullptr;
		inputVertexShader = nullptr;
		inputPixelShader = nullptr;
		inputParamsBuffer = nullptr;
		hdrSourceSrv = nullptr;
		hdrSourceTexture = nullptr;
		for (GpuQuerySlot& slot : gpuQuerySlots) {
			slot.disjoint = nullptr;
			slot.start = nullptr;
			slot.end = nullptr;
			slot.pending = false;
		}
		gpuTimingWindow = {};
		nextGpuQuerySlot = 0;
		gpuTimingSampleCount = 0;
		gpuTimingAvailable = false;
		profileLabel = {};
		inputDxgiFormat = DXGI_FORMAT_UNKNOWN;
		inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
		gridSize = 0;
		costEnabled = false;
		previousSlot = 0;
		bidirectional = false;
		historyValid = false;
		resources = nullptr;
		device = nullptr;
		context = nullptr;
		extent = {};
		analysisExtent = {};
	}

	bool CreateGpuTimingQueries() noexcept {
		D3D11_QUERY_DESC desc{ .Query = D3D11_QUERY_TIMESTAMP_DISJOINT };
		for (GpuQuerySlot& slot : gpuQuerySlots) {
			if (FAILED(device->CreateQuery(&desc, slot.disjoint.put()))) return false;
			desc.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&desc, slot.start.put())) ||
				FAILED(device->CreateQuery(&desc, slot.end.put()))) return false;
			desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		}
		return true;
	}

	void PollGpuTimings() noexcept {
		for (GpuQuerySlot& slot : gpuQuerySlots) {
			if (!slot.pending) continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			if (context->GetData(
				slot.disjoint.get(), &disjoint, sizeof(disjoint),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
			uint64_t start = 0;
			uint64_t end = 0;
			const bool ready = context->GetData(
				slot.start.get(), &start, sizeof(start),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
				context->GetData(
					slot.end.get(), &end, sizeof(end),
					D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
			if (!ready) continue;
			slot.pending = false;
			if (disjoint.Disjoint || !disjoint.Frequency || end < start) continue;
			gpuTimingWindow.Add(
				double(end - start) * 1000.0 / double(disjoint.Frequency));
			++gpuTimingSampleCount;
			if (gpuTimingSampleCount <= 4 || gpuTimingSampleCount % 120 == 0) {
				const NvofTimingSummary timing = gpuTimingWindow.Summarize();
				Logger::Get().Info(fmt::format(
					"Frame Guidance NVOF GPU interval: profile={} samples={} avg={:.3f} ms "
					"p95={:.3f} ms p99={:.3f} ms max={:.3f} ms",
					profileLabel, timing.count, timing.average, timing.p95,
					timing.p99, timing.maximum));
			}
		}
	}

	GpuQuerySlot* BeginGpuTiming() noexcept {
		if (!gpuTimingAvailable) return nullptr;
		PollGpuTimings();
		GpuQuerySlot& slot =
			gpuQuerySlots[nextGpuQuerySlot++ % GPU_QUERY_SLOT_COUNT];
		if (slot.pending) return nullptr;
		context->Begin(slot.disjoint.get());
		context->End(slot.start.get());
		return &slot;
	}

	void EndGpuTiming(GpuQuerySlot* slot) noexcept {
		if (!slot) return;
		context->End(slot->end.get());
		context->End(slot->disjoint.get());
		slot->pending = true;
	}

	bool QueryFormats(
		NV_OF_BUFFER_USAGE usage,
		std::vector<DXGI_FORMAT>& formats
	) noexcept {
		uint32_t count = 0;
		if (api.nvOFGetSurfaceFormatCountD3D11(
			session, usage, NV_OF_MODE_OPTICALFLOW, &count) != NV_OF_SUCCESS ||
			count == 0 || count > 64) {
			return false;
		}
		formats.resize(count);
		return api.nvOFGetSurfaceFormatD3D11(
			session, usage, NV_OF_MODE_OPTICALFLOW,
			formats.data()) == NV_OF_SUCCESS;
	}

	std::vector<uint32_t> QueryCaps(NV_OF_CAPS cap) noexcept {
		uint32_t count = 0;
		if (!api.nvOFGetCaps ||
			api.nvOFGetCaps(session, cap, nullptr, &count) != NV_OF_SUCCESS ||
			count == 0 || count > 64) {
			return {};
		}
		std::vector<uint32_t> values(count);
		if (api.nvOFGetCaps(session, cap, values.data(), &count) != NV_OF_SUCCESS) {
			return {};
		}
		if (count > values.size()) return {};
		values.resize(count);
		return values;
	}

	bool Register(ID3D11Resource* resource, NvOFGPUBufferHandle& handle) noexcept {
		return api.nvOFRegisterResourceD3D11 &&
			api.nvOFRegisterResourceD3D11(
				session, resource, &handle) == NV_OF_SUCCESS && handle;
	}

	bool CreateTextures() noexcept {
		const UINT sourceBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		for (auto& texture : input) {
			texture = DirectXHelper::CreateTexture2D(
				device, inputDxgiFormat, analysisExtent.width, analysisExtent.height,
				sourceBind);
			if (!texture || FAILED(device->CreateRenderTargetView(texture.get(), nullptr,
				inputRtv[&texture - &input[0]].put()))) return false;
		}

		const uint32_t flowWidth = (analysisExtent.width + gridSize - 1) / gridSize;
		const uint32_t flowHeight = (analysisExtent.height + gridSize - 1) / gridSize;
		for (size_t i = 0; i < flow.size(); ++i) {
			flow[i] = DirectXHelper::CreateTexture2D(
				device, DXGI_FORMAT_R16G16_SINT, flowWidth, flowHeight,
				D3D11_BIND_SHADER_RESOURCE);
			if (!flow[i] || FAILED(device->CreateShaderResourceView(
				flow[i].get(), nullptr, flowSrv[i].put()))) {
				return false;
			}
		}
		if (costEnabled) {
			for (size_t i = 0; i < cost.size(); ++i) {
				cost[i] = DirectXHelper::CreateTexture2D(
					device, DXGI_FORMAT_R8_UINT, flowWidth, flowHeight,
					D3D11_BIND_SHADER_RESOURCE);
				if (!cost[i] || FAILED(device->CreateShaderResourceView(
					cost[i].get(), nullptr, costSrv[i].put()))) {
					return false;
				}
			}
		}

		constexpr UINT GUIDE_BIND =
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		motion = DirectXHelper::CreateSharedTexture2D(
			device, DXGI_FORMAT_R16G16_FLOAT, extent.width, extent.height,
			GUIDE_BIND, "NVOF/Motion");
		confidence = DirectXHelper::CreateSharedTexture2D(
			device, DXGI_FORMAT_R8_UNORM, extent.width, extent.height,
			GUIDE_BIND, "NVOF/Confidence");
		if (!motion || !confidence ||
			FAILED(device->CreateUnorderedAccessView(
				motion.get(), nullptr, motionUav.put())) ||
			FAILED(device->CreateUnorderedAccessView(
				confidence.get(), nullptr, confidenceUav.put()))) {
			return false;
		}

		for (size_t i = 0; i < input.size(); ++i) {
			if (!Register(input[i].get(), registered[i])) return false;
		}
		for (size_t i = 0; i < flow.size(); ++i) {
			if (!Register(flow[i].get(), registered[2 + i])) return false;
		}
		if (costEnabled) {
			for (size_t i = 0; i < cost.size(); ++i) {
				if (!Register(cost[i].get(), registered[4 + i])) return false;
			}
		}
		return true;
	}

	bool CreatePostProcess() noexcept {
		winrt::com_ptr<ID3DBlob> vs, ps, errors;
		HRESULT hr = D3DCompile(NVOF_INPUT_HLSL, sizeof(NVOF_INPUT_HLSL) - 1,
			"NVOF/Input", nullptr, nullptr, "InputVS", "vs_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vs.put(), errors.put());
		if (FAILED(hr)) return false;
		errors = nullptr;
		hr = D3DCompile(NVOF_INPUT_HLSL, sizeof(NVOF_INPUT_HLSL) - 1,
			"NVOF/Input", nullptr, nullptr, "InputPS", "ps_5_0",
			D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ps.put(), errors.put());
		if (FAILED(hr) || FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
			nullptr, inputVertexShader.put())) || FAILED(device->CreatePixelShader(ps->GetBufferPointer(),
			ps->GetBufferSize(), nullptr, inputPixelShader.put()))) return false;
		D3D11_BUFFER_DESC inputDesc{ .ByteWidth = 16, .Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE };
		if (FAILED(device->CreateBuffer(&inputDesc, nullptr, inputParamsBuffer.put()))) return false;
		winrt::com_ptr<ID3DBlob> shaderBlob;
		if (!DirectXHelper::CompileComputeShader(
			DENSIFY_FLOW_HLSL, "Densify", shaderBlob.put(),
			"FrameGuidance/NVOF_Densify.hlsl")) {
			return false;
		}
		if (FAILED(device->CreateComputeShader(
			shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
			densifyShader.put()))) {
			return false;
		}
		const D3D11_BUFFER_DESC desc{
			.ByteWidth = 48,
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
		};
		return SUCCEEDED(device->CreateBuffer(&desc, nullptr, paramsBuffer.put()));
	}

	bool CreateSession(
		DeviceResources& deviceResources,
		FrameGuidanceExtent newExtent,
		NvidiaOpticalFlowQuality requestedQuality, uint32_t resolutionPercent
	) noexcept {
		DestroySession();
		initializationError = OpticalFlowInitializationError::ProviderUnavailable;
		bool initialized = false;
		auto rollback = wil::scope_exit([&]() noexcept {
			if (initialized) return;
			const OpticalFlowInitializationError error = initializationError;
			DestroySession();
			initializationError = error;
		});
		resources = &deviceResources;
		device = deviceResources.GetD3DDevice();
		context = deviceResources.GetD3DDC();
		extent = newExtent;
		if (!device || !context || !extent.IsValid()) return false;
		DXGI_ADAPTER_DESC1 adapterDesc{};
		if (!deviceResources.GetGraphicsAdapter() || FAILED(
			deviceResources.GetGraphicsAdapter()->GetDesc1(&adapterDesc)) ||
			adapterDesc.VendorId != 0x10DE) {
			Logger::Get().Warn(
				"NVOF unavailable: selected adapter is not an NVIDIA adapter; "
				"select AMD OF or None");
			return false;
		}

		module = LoadLibraryExW(
			L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!module) {
			Logger::Get().Warn(
				"NVOF unavailable: driver nvofapi64.dll was not found");
			return false;
		}
		auto createApi = GetExport<CreateApiFn>(
			module, "NvOFAPICreateInstanceD3D11");
		if (!createApi) return false;
		uint32_t driverVersion = NV_OF_API_VERSION;
		if (auto getMaxVersion = GetExport<GetMaxVersionFn>(
			module, "NvOFGetMaxSupportedApiVersion")) {
			if (getMaxVersion(&driverVersion) != NV_OF_SUCCESS ||
				driverVersion < NV_OF_API_VERSION) {
				Logger::Get().Warn("NVOF unavailable: NVIDIA driver API is too old");
				return false;
			}
		}
		if (createApi(NV_OF_API_VERSION, &api) != NV_OF_SUCCESS ||
			!api.nvCreateOpticalFlowD3D11 || !api.nvOFInit ||
			!api.nvOFExecute || !api.nvOFRegisterResourceD3D11 ||
			!api.nvOFUnregisterResourceD3D11) {
			return false;
		}
		if (api.nvCreateOpticalFlowD3D11(
			device, context, &session) != NV_OF_SUCCESS || !session) {
			return false;
		}

		std::vector<DXGI_FORMAT> inputFormats;
		std::vector<DXGI_FORMAT> outputFormats;
		std::vector<DXGI_FORMAT> costFormats;
		const bool costFormatAvailable =
			QueryFormats(NV_OF_BUFFER_USAGE_COST, costFormats) &&
			HasFormat(costFormats, DXGI_FORMAT_R8_UINT);
		if (!QueryFormats(NV_OF_BUFFER_USAGE_INPUT, inputFormats) ||
			!QueryFormats(NV_OF_BUFFER_USAGE_OUTPUT, outputFormats) ||
			!HasFormat(outputFormats, DXGI_FORMAT_R16G16_SINT)) {
			Logger::Get().Warn("NVOF required S10.5 output format unavailable");
			return false;
		}
		if (HasFormat(inputFormats, DXGI_FORMAT_B8G8R8A8_UNORM)) {
			inputDxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
		} else if (HasFormat(inputFormats, DXGI_FORMAT_R8G8B8A8_UNORM)) {
			inputDxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		} else {
			Logger::Get().Warn("NVOF renderable ABGR8 input format unavailable");
			return false;
		}
		inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
		auto minimum = [&](NV_OF_CAPS cap) -> uint32_t {
			const auto values = QueryCaps(cap);
			return values.size() == 1 ? std::max(values[0], 1u) : 1u;
		};
		analysisExtent = {
			OpticalFlowAnalysisDimension(extent.width, resolutionPercent, minimum(NV_OF_CAPS_WIDTH_MIN)),
			OpticalFlowAnalysisDimension(extent.height, resolutionPercent, minimum(NV_OF_CAPS_HEIGHT_MIN))
		};

		const std::vector<uint32_t> grids = QueryCaps(
			NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES);
		NvofProfile profile = ResolveProfile(requestedQuality);
		if (profile.gridSize == 2 && std::find(grids.begin(), grids.end(), 2u) == grids.end() &&
			std::find(grids.begin(), grids.end(), 4u) != grids.end()) {
			Logger::Get().Warn(fmt::format("NVOF 2x2 grid unavailable: requested={} using 4x4 at the same performance level", profile.label));
			profile.gridSize = 4;
			profile.label = requestedQuality == NvidiaOpticalFlowQuality::HighestQuality ? "4S-fallback" : "4M-fallback";
		}
		if (std::find(grids.begin(), grids.end(), profile.gridSize) == grids.end()) {
			initializationError = OpticalFlowInitializationError::QualityUnsupported;
			Logger::Get().Warn(fmt::format(
				"NVOF quality unsupported: profile={} requires {}x{} output grid; "
				"select a lower quality, AMD OF, or None",
				profile.label, profile.gridSize, profile.gridSize));
			return false;
		}
		gridSize = profile.gridSize;
		profileLabel = profile.label;
		initializationError = OpticalFlowInitializationError::InteropFailed;

		NV_OF_INIT_PARAMS init{
			.width = analysisExtent.width,
			.height = analysisExtent.height,
			.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(gridSize),
			.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED,
			.mode = NV_OF_MODE_OPTICALFLOW,
			.perfLevel = profile.perfLevel,
			.enableExternalHints = NV_OF_FALSE,
			.enableOutputCost = costFormatAvailable ? NV_OF_TRUE : NV_OF_FALSE,
			.hPrivData = nullptr,
			.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED,
			.enableRoi = NV_OF_FALSE,
			.predDirection = NV_OF_PRED_DIRECTION_BOTH,
			.enableGlobalFlow = NV_OF_FALSE,
			.inputBufferFormat = inputBufferFormat
		};
		NV_OF_STATUS status = api.nvOFInit(session, &init);
		bidirectional = status == NV_OF_SUCCESS;
		costEnabled = costFormatAvailable && status == NV_OF_SUCCESS;
		if (status != NV_OF_SUCCESS && costFormatAvailable) {
			init.enableOutputCost = NV_OF_FALSE;
			status = api.nvOFInit(session, &init);
			bidirectional = status == NV_OF_SUCCESS;
			costEnabled = false;
		}
		if (status != NV_OF_SUCCESS) {
			init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
			init.enableOutputCost = costFormatAvailable ? NV_OF_TRUE : NV_OF_FALSE;
			status = api.nvOFInit(session, &init);
			costEnabled = costFormatAvailable && status == NV_OF_SUCCESS;
			if (status != NV_OF_SUCCESS && costFormatAvailable) {
				init.enableOutputCost = NV_OF_FALSE;
				status = api.nvOFInit(session, &init);
				costEnabled = false;
			}
			bidirectional = false;
		}
		if (status != NV_OF_SUCCESS || !CreateTextures() || !CreatePostProcess()) {
			return false;
		}
		gpuTimingAvailable = NativeBackendTiming::Enabled && CreateGpuTimingQueries();
		if constexpr (NativeBackendTiming::Enabled) {
			if (!gpuTimingAvailable) {
				Logger::Get().Warn(
					"Frame Guidance NVOF GPU timing unavailable; continuing without telemetry");
			}
		}

		resetReason = FrameGuidanceResetReason::Initialize;
		temporalHistory.Reset();
		historyValid = false;
		previousSlot = 0;
		Logger::Get().Info(fmt::format(
			"Frame Guidance NVOF initialized: profile={} grid={}x{} perf={} "
			"driverApi={}.{} cost={} bidirectional={} source={}x{} analysis={}x{} ({}%) output={}x{} "
			"convention=current-to-previous/source-pixels/S10.5-self-test-passed",
			profile.label, gridSize, gridSize, profile.perfLabel,
			driverVersion >> 4, driverVersion & 0xf,
			costEnabled ? "R8_UINT" : "none", bidirectional,
			extent.width, extent.height, analysisExtent.width, analysisExtent.height, resolutionPercent,
			(analysisExtent.width + gridSize - 1) / gridSize,
			(analysisExtent.height + gridSize - 1) / gridSize));
		initializationError = OpticalFlowInitializationError::None;
		initialized = true;
		return true;
	}

	void ClearDenseOutput() noexcept {
		static constexpr float ZERO[4]{};
		context->ClearUnorderedAccessViewFloat(motionUav.get(), ZERO);
		context->ClearUnorderedAccessViewFloat(confidenceUav.get(), ZERO);
	}

	bool Densify() noexcept {
		struct alignas(16) Params {
			uint32_t sourceWidth;
			uint32_t sourceHeight;
			uint32_t analysisWidth;
			uint32_t analysisHeight;
			uint32_t flowWidth;
			uint32_t flowHeight;
			uint32_t gridSize;
			uint32_t hasForwardCost;
			uint32_t hasBackward;
			uint32_t hasBackwardCost;
			uint32_t reserved[2];
		};
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(
			paramsBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		*static_cast<Params*>(mapped.pData) = {
			extent.width, extent.height, analysisExtent.width, analysisExtent.height,
			(analysisExtent.width + gridSize - 1) / gridSize,
			(analysisExtent.height + gridSize - 1) / gridSize,
			gridSize, costEnabled ? 1u : 0u,
			bidirectional ? 1u : 0u,
			bidirectional && costEnabled ? 1u : 0u, { 0, 0 }
		};
		context->Unmap(paramsBuffer.get(), 0);

		ID3D11ShaderResourceView* srvs[]{
			flowSrv[0].get(), bidirectional ? flowSrv[1].get() : nullptr,
			costSrv[0].get(), bidirectional ? costSrv[1].get() : nullptr
		};
		ID3D11UnorderedAccessView* uavs[]{ motionUav.get(), confidenceUav.get() };
		ID3D11Buffer* buffers[]{ paramsBuffer.get() };
		context->CSSetShader(densifyShader.get(), nullptr, 0);
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		context->CSSetConstantBuffers(0, 1, buffers);
		context->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSrvs[ARRAYSIZE(srvs)]{};
		ID3D11UnorderedAccessView* nullUavs[ARRAYSIZE(uavs)]{};
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		return true;
	}

	DeviceResources* resources = nullptr;
	ID3D11Device5* device = nullptr;
	ID3D11DeviceContext4* context = nullptr;
	HMODULE module = nullptr;
	NV_OF_D3D11_API_FUNCTION_LIST api{};
	NvOFHandle session = nullptr;
	std::array<NvOFGPUBufferHandle, 8> registered{};
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> input;
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> flow;
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> cost;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> flowSrv;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> costSrv;
	winrt::com_ptr<ID3D11Texture2D> motion;
	winrt::com_ptr<ID3D11Texture2D> confidence;
	winrt::com_ptr<ID3D11UnorderedAccessView> motionUav;
	winrt::com_ptr<ID3D11UnorderedAccessView> confidenceUav;
	winrt::com_ptr<ID3D11ComputeShader> densifyShader;
	winrt::com_ptr<ID3D11Buffer> paramsBuffer;
	std::array<GpuQuerySlot, GPU_QUERY_SLOT_COUNT> gpuQuerySlots;
	NvofTimingWindow gpuTimingWindow;
	FrameGuidanceExtent extent{}, analysisExtent{};
	FrameGuidanceResetReason resetReason = FrameGuidanceResetReason::Initialize;
	uint32_t nextGpuQuerySlot = 0;
	uint32_t gridSize = 0;
	uint32_t previousSlot = 0;
	uint64_t gpuTimingSampleCount = 0;
	std::string_view profileLabel;
	bool bidirectional = false;
	std::array<winrt::com_ptr<ID3D11RenderTargetView>, 2> inputRtv;
	winrt::com_ptr<ID3D11ShaderResourceView> hdrSourceSrv;
	ID3D11Texture2D* hdrSourceTexture = nullptr;
	winrt::com_ptr<ID3D11VertexShader> inputVertexShader;
	winrt::com_ptr<ID3D11PixelShader> inputPixelShader;
	winrt::com_ptr<ID3D11Buffer> inputParamsBuffer;
	bool costEnabled = false;
	bool gpuTimingAvailable = false;
	OpticalFlowHistory temporalHistory;
	bool historyValid = false;
	OpticalFlowInitializationError initializationError =
		OpticalFlowInitializationError::ProviderUnavailable;
	DXGI_FORMAT inputDxgiFormat = DXGI_FORMAT_UNKNOWN;
	NV_OF_BUFFER_FORMAT inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
};

NvidiaOpticalFlowProvider::NvidiaOpticalFlowProvider(
	NvidiaOpticalFlowQuality quality, uint32_t resolutionPercent
) : _quality(quality), _resolutionPercent(std::clamp(resolutionPercent, 25u, 100u)), _impl(std::make_unique<Impl>()) {}

NvidiaOpticalFlowProvider::~NvidiaOpticalFlowProvider() = default;

bool NvidiaOpticalFlowProvider::Initialize(
	DeviceResources& resources,
	FrameGuidanceExtent sourceExtent
) noexcept {
	return _impl->CreateSession(resources, sourceExtent, _quality, _resolutionPercent);
}

bool NvidiaOpticalFlowProvider::BeginFrame(
	const FrameGuidanceFrame& frame,
	MotionVectorProviderOutput& output
) noexcept {
	FrameTrace::Scope traceNvof(FrameTrace::Event::NvofSubmit, static_cast<int64_t>(_quality));
	Impl& impl = *_impl;
	if (!frame.color || frame.sourceExtent != impl.extent || !impl.session) {
		return false;
	}
	D3D11_TEXTURE2D_DESC frameDesc{};
	frame.color->GetDesc(&frameDesc);
	if (frameDesc.Width != impl.extent.width || frameDesc.Height != impl.extent.height ||
		(frameDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && frameDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
		 frameDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)) return false;
	const uint32_t currentSlot = impl.historyValid ? 1u - impl.previousSlot : 0u;
	if (impl.hdrSourceTexture != frame.color) {
		impl.hdrSourceSrv = nullptr;
		if (FAILED(impl.device->CreateShaderResourceView(frame.color, nullptr, impl.hdrSourceSrv.put()))) return false;
		impl.hdrSourceTexture = frame.color;
	}
	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(impl.context->Map(impl.inputParamsBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
	const uint32_t conversionParams[]{ impl.analysisExtent.width, impl.analysisExtent.height,
		frameDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1u : 0u, 0u };
	memcpy(mapped.pData, conversionParams, sizeof(conversionParams));
	impl.context->Unmap(impl.inputParamsBuffer.get(), 0);
	// A render target supports BGRA on D3D11 devices that lack typed BGRA UAVs.
	// The renderer clears state again before running its effect chain.
	impl.context->ClearState();
	ID3D11ShaderResourceView* srv = impl.hdrSourceSrv.get();
	ID3D11RenderTargetView* rtv = impl.inputRtv[currentSlot].get();
	ID3D11Buffer* cb = impl.inputParamsBuffer.get();
	ID3D11SamplerState* sampler = impl.resources->GetSampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP);
	if (!sampler) return false;
	impl.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	impl.context->VSSetShader(impl.inputVertexShader.get(), nullptr, 0);
	impl.context->PSSetShader(impl.inputPixelShader.get(), nullptr, 0);
	impl.context->PSSetShaderResources(0, 1, &srv);
	impl.context->PSSetSamplers(0, 1, &sampler);
	impl.context->PSSetConstantBuffers(0, 1, &cb);
	impl.context->OMSetRenderTargets(1, &rtv, nullptr);
	const D3D11_VIEWPORT viewport{ 0, 0, float(impl.analysisExtent.width), float(impl.analysisExtent.height), 0, 1 };
	impl.context->RSSetViewports(1, &viewport);
	impl.context->Draw(3, 0);
	impl.context->ClearState();

	if (!impl.historyValid) {
		impl.ClearDenseOutput();
		impl.previousSlot = currentSlot;
		impl.historyValid = true;
	} else {
		NV_OF_EXECUTE_INPUT_PARAMS inputParams{
			.inputFrame = impl.registered[currentSlot],
			.referenceFrame = impl.registered[impl.previousSlot],
			.disableTemporalHints = !impl.temporalHistory.DisableTemporalHints() ?
				NV_OF_FALSE : NV_OF_TRUE
		};
		NV_OF_EXECUTE_OUTPUT_PARAMS outputParams{
			.outputBuffer = impl.registered[2],
			.outputCostBuffer = impl.costEnabled ? impl.registered[4] : nullptr,
			.bwdOutputBuffer = impl.bidirectional ? impl.registered[3] : nullptr,
			.bwdOutputCostBuffer = impl.bidirectional && impl.costEnabled ?
				impl.registered[5] : nullptr
		};
		const auto begin = NativeBackendTiming::Now();
		Impl::GpuQuerySlot* gpuTiming = impl.BeginGpuTiming();
		const bool succeeded = impl.api.nvOFExecute(
			impl.session, &inputParams, &outputParams) == NV_OF_SUCCESS &&
			impl.Densify();
		impl.EndGpuTiming(gpuTiming);
		if (!succeeded) {
			impl.historyValid = false;
			impl.resetReason = FrameGuidanceResetReason::ProviderFailure;
			impl.temporalHistory.Reset();
			Logger::Get().Warn(fmt::format(
				"Frame Guidance NVOF failed at frameId={}", frame.frameId));
			return false;
		}
		impl.previousSlot = currentSlot;
		impl.temporalHistory.Executed();
		const auto elapsed = NativeBackendTiming::ElapsedMilliseconds(begin);
		if constexpr (NativeBackendTiming::Enabled) {
			if (frame.frameId <= 2) {
				Logger::Get().Info(fmt::format(
					"Frame Guidance NVOF submit+dense frameId={} CPU={:.3f} ms",
					frame.frameId, elapsed));
			}
		}
	}

	const bool isZero = impl.resetReason != FrameGuidanceResetReason::None;
	const FrameGuidanceMetadata metadata = MakeMetadata(
		frame, impl.resetReason, isZero);
	output.motion = {
		.texture = impl.motion.get(),
		.format = DXGI_FORMAT_R16G16_FLOAT,
		.metadata = metadata
	};
	output.confidence = {
		.texture = impl.confidence.get(),
		.format = DXGI_FORMAT_R8_UNORM,
		.metadata = metadata
	};
	impl.resetReason = FrameGuidanceResetReason::None;
	return true;
}

void NvidiaOpticalFlowProvider::Reset(
	FrameGuidanceResetReason reason
) noexcept {
	_impl->historyValid = false;
	_impl->temporalHistory.Reset();
	_impl->resetReason = reason;
}

bool NvidiaOpticalFlowProvider::Resize(
	FrameGuidanceExtent sourceExtent
) noexcept {
	if (!_impl->resources) return false;
	const bool result = _impl->CreateSession(
		*_impl->resources, sourceExtent, _quality, _resolutionPercent);
	if (result) _impl->resetReason = FrameGuidanceResetReason::Resize;
	return result;
}

OpticalFlowInitializationError
NvidiaOpticalFlowProvider::InitializationError() const noexcept {
	return _impl->initializationError;
}

}

#else

namespace Magpie {

struct NvidiaOpticalFlowProvider::Impl {
	OpticalFlowInitializationError initializationError =
		OpticalFlowInitializationError::ProviderUnavailable;
};

NvidiaOpticalFlowProvider::NvidiaOpticalFlowProvider(
	NvidiaOpticalFlowQuality quality, uint32_t resolutionPercent
) : _quality(quality), _resolutionPercent(std::clamp(resolutionPercent, 25u, 100u)), _impl(std::make_unique<Impl>()) {}
NvidiaOpticalFlowProvider::~NvidiaOpticalFlowProvider() = default;
bool NvidiaOpticalFlowProvider::Initialize(
	DeviceResources&, FrameGuidanceExtent) noexcept { return false; }
bool NvidiaOpticalFlowProvider::BeginFrame(
	const FrameGuidanceFrame&, MotionVectorProviderOutput&) noexcept { return false; }
void NvidiaOpticalFlowProvider::Reset(FrameGuidanceResetReason) noexcept {}
bool NvidiaOpticalFlowProvider::Resize(FrameGuidanceExtent) noexcept { return false; }
OpticalFlowInitializationError
NvidiaOpticalFlowProvider::InitializationError() const noexcept {
	return _impl->initializationError;
}

}

#endif
