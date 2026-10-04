#include "RtxVideoBridge.h"
#include <d3d10.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers_truehdr.h>
#include <new>

namespace {
// Private copy of the RTX Video SDK loader. DLSS keeps its own newer NGX
// loader in Magpie.exe. Magpie calls only our plain ABI, not the SDK exports.
ID3D11Device* activeDevice = nullptr;
unsigned users = 0;
bool faulted = false;
struct Instance {
	ID3D11DeviceContext* context = nullptr;
	NVSDK_NGX_Parameter* parameters = nullptr;
	NVSDK_NGX_Handle* feature = nullptr;
};

void DestroyImpl(Instance* state) {
	if (!state) return;
	if (!faulted) {
		if (state->feature) NVSDK_NGX_D3D11_ReleaseFeature(state->feature);
		if (state->parameters) NVSDK_NGX_D3D11_DestroyParameters(state->parameters);
	}
	if (state->context) state->context->Release();
	delete state;
	if (users && --users == 0 && activeDevice) {
		if (!faulted) NVSDK_NGX_D3D11_Shutdown1(activeDevice);
		activeDevice->Release();
		activeDevice = nullptr;
	}
}

HRESULT CreateImpl(ID3D11Device* device, const wchar_t* path, void** output, uint32_t* status) {
	if (!device || !path || !output || !status) return E_INVALIDARG;
	*output = nullptr;
	*status = 0;
	if (faulted || (activeDevice && activeDevice != device)) return E_UNEXPECTED;
	if (!users) {
		ID3D10Multithread* protection = nullptr;
		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&protection)))) {
			protection->SetMultithreadProtected(TRUE);
			protection->Release();
		}
		const wchar_t* paths[] = { path };
		NVSDK_NGX_FeatureCommonInfo common{};
		common.PathListInfo.Path = paths;
		common.PathListInfo.Length = 1;
		const auto result = NVSDK_NGX_D3D11_Init(0, path, device, &common);
		*status = static_cast<uint32_t>(result);
		if (NVSDK_NGX_FAILED(result)) return E_FAIL;
		activeDevice = device;
		activeDevice->AddRef();
	}
	++users;
	auto* state = new(std::nothrow) Instance;
	if (!state) {
		if (--users == 0) {
			NVSDK_NGX_D3D11_Shutdown1(activeDevice);
			activeDevice->Release(); activeDevice = nullptr;
		}
		return E_OUTOFMEMORY;
	}
	device->GetImmediateContext(&state->context);
	auto result = NVSDK_NGX_D3D11_GetCapabilityParameters(&state->parameters);
	int available = 0;
	if (NVSDK_NGX_SUCCEED(result)) result = state->parameters->Get(NVSDK_NGX_Parameter_TrueHDR_Available, &available);
	if (NVSDK_NGX_SUCCEED(result) && available) {
		NVSDK_NGX_Feature_Create_Params create{};
		result = NGX_D3D11_CREATE_TRUEHDR_EXT(state->context, &state->feature, state->parameters, &create);
	} else if (NVSDK_NGX_SUCCEED(result)) {
		result = NVSDK_NGX_Result_FAIL_FeatureNotSupported;
	}
	*status = static_cast<uint32_t>(result);
	if (NVSDK_NGX_FAILED(result)) { DestroyImpl(state); return E_FAIL; }
	*output = state;
	return S_OK;
}

HRESULT DrawImpl(Instance* state, ID3D11Texture2D* input, ID3D11Texture2D* output,
	const MagpieRtxHdrSettings* settings, uint32_t* status) {
	if (faulted || !state || !input || !output || !settings || !status) return E_INVALIDARG;
	*status = 0;
	D3D11_TEXTURE2D_DESC in{}, out{};
	input->GetDesc(&in); output->GetDesc(&out);
	if (!MagpieRtxHdrEndpointsSupported(in, out) ||
		settings->contrast > 200 || settings->saturation > 200 || settings->middleGray < 10 || settings->middleGray > 100 ||
		settings->maxLuminance < 400 || settings->maxLuminance > 2000) return E_INVALIDARG;
	NVSDK_NGX_D3D11_TRUEHDR_Eval_Params eval{};
	eval.pInput = input; eval.pOutput = output;
	eval.InputSubrectBR.Width = in.Width; eval.InputSubrectBR.Height = in.Height;
	eval.OutputSubrectBR.Width = out.Width; eval.OutputSubrectBR.Height = out.Height;
	eval.Contrast = settings->contrast; eval.Saturation = settings->saturation;
	eval.MiddleGray = settings->middleGray; eval.MaxLuminance = settings->maxLuminance;
	const auto result = NGX_D3D11_EVALUATE_TRUEHDR_EXT(state->context, state->feature, state->parameters, &eval);
	*status = static_cast<uint32_t>(result);
	return NVSDK_NGX_SUCCEED(result) ? S_OK : E_FAIL;
}
}

extern "C" __declspec(dllexport) HRESULT WINAPI MagpieRtxHdrCreate(
	ID3D11Device* device, const wchar_t* path, void** output, uint32_t* status) {
	__try { return CreateImpl(device, path, output, status); }
	__except(EXCEPTION_EXECUTE_HANDLER) {
		faulted = true; if (status) *status = GetExceptionCode(); return E_UNEXPECTED;
	}
}
extern "C" __declspec(dllexport) HRESULT WINAPI MagpieRtxHdrDraw(void* state,
	ID3D11Texture2D* input, ID3D11Texture2D* output, const MagpieRtxHdrSettings* settings, uint32_t* status) {
	__try { return DrawImpl(static_cast<Instance*>(state), input, output, settings, status); }
	__except(EXCEPTION_EXECUTE_HANDLER) {
		faulted = true; if (status) *status = GetExceptionCode(); return E_UNEXPECTED;
	}
}
extern "C" __declspec(dllexport) HRESULT WINAPI MagpieRtxHdrDestroy(void* state) {
	__try { DestroyImpl(static_cast<Instance*>(state)); return faulted ? E_UNEXPECTED : S_OK; }
	__except(EXCEPTION_EXECUTE_HANDLER) { faulted = true; return E_UNEXPECTED; }
}
