#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <limits>

namespace Magpie {

// A GPU completion signal for the final colour/UI/cursor image, not a driver
// VRR query. Polling and event waits are nonblocking in the renderer. The D3D11
// context is owned by the frontend; no inference/Reflex/NGX callback is entered.
class PresentationReadyFence {
public:
	PresentationReadyFence() = default;
	~PresentationReadyFence() { if (_event) CloseHandle(_event); }
	PresentationReadyFence(const PresentationReadyFence&) = delete;
	PresentationReadyFence& operator=(const PresentationReadyFence&) = delete;
	HRESULT Initialize(ID3D11Device5* device) noexcept {
		HRESULT hr = device->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(_fence.ReleaseAndGetAddressOf()));
		if (FAILED(hr)) return hr;
		_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		return _event ? S_OK : HRESULT_FROM_WIN32(GetLastError());
	}
	HRESULT Mark(ID3D11DeviceContext4* context) noexcept {
		if (!_fence || !_event || _pending) return E_UNEXPECTED;
		ResetEvent(_event);
		_target = ++_value;
		HRESULT hr = context->Signal(_fence.Get(), _target);
		if (SUCCEEDED(hr)) hr = _fence->SetEventOnCompletion(_target, _event);
		if (FAILED(hr)) return hr;
		_pending = true;
		context->Flush();
		return S_OK;
	}
	HRESULT Poll() noexcept {
		if (!_pending) return S_OK;
		const uint64_t completed = _fence->GetCompletedValue();
		if (completed == std::numeric_limits<uint64_t>::max()) return DXGI_ERROR_DEVICE_REMOVED;
		if (completed < _target) return S_FALSE;
		_pending = false;
		return S_OK;
	}
	bool Pending() const noexcept { return _pending; }
	HANDLE Event() const noexcept { return _pending ? _event : nullptr; }
	// Keep the monotonically increasing value across resize/cancellation. A late
	// old event cannot make a new image pass Poll before its own fence completes.
	void Cancel() noexcept { _pending = false; }
private:
	Microsoft::WRL::ComPtr<ID3D11Fence> _fence;
	HANDLE _event = nullptr;
	uint64_t _value = 0, _target = 0;
	bool _pending = false;
};

}
