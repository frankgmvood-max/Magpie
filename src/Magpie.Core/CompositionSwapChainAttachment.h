#pragma once
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace Magpie {

// A DXGI flip swap chain attached to a DComp visual, not a DComp virtual surface.
// Present(0, ALLOW_TEARING) still owns every image. The layered host can retain
// native mouse transparency without cutting a region out of the output image.
class CompositionSwapChainAttachment {
public:
	HRESULT Initialize(HWND window, IDXGISwapChain1* swapChain) noexcept {
		Reset();
		HRESULT hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&_device));
		if (SUCCEEDED(hr)) hr = _device->CreateTargetForHwnd(window, TRUE, &_target);
		if (SUCCEEDED(hr)) hr = _device->CreateVisual(&_visual);
		if (SUCCEEDED(hr)) hr = _visual->SetContent(swapChain);
		if (SUCCEEDED(hr)) hr = _target->SetRoot(_visual.Get());
		if (SUCCEEDED(hr)) hr = _device->Commit();
		if (FAILED(hr)) Reset();
		return hr;
	}
	void Reset() noexcept {
		if (_visual) _visual->SetContent(nullptr);
		if (_target) _target->SetRoot(nullptr);
		if (_device) _device->Commit();
		_visual.Reset(); _target.Reset(); _device.Reset();
	}
	~CompositionSwapChainAttachment() { Reset(); }
private:
	Microsoft::WRL::ComPtr<IDCompositionDevice> _device;
	Microsoft::WRL::ComPtr<IDCompositionTarget> _target;
	Microsoft::WRL::ComPtr<IDCompositionVisual> _visual;
};

}
