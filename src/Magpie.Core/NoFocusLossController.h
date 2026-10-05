#pragma once
#include "include/NoFocusLossApi.h"
#include "include/NoFocusLossSettings.h"

namespace Magpie {

struct NoFocusLossObservation {
	HWND actual = nullptr;
	HWND perceived = nullptr;
	HWND output = nullptr;
	uint64_t calls = 0;
	uint64_t spoofed = 0;
	uint64_t suppressedMessages = 0;
	bool shouldSpoof = false;
	bool subclassRegistered = false;
};

class NoFocusLossController final {
public:
	~NoFocusLossController() noexcept { Stop(); }
	NoFocusLossController() = default;
	NoFocusLossController(const NoFocusLossController&) = delete;
	NoFocusLossController& operator=(const NoFocusLossController&) = delete;

	// Called only after successful initialization and the first visible frame.
	bool Start(HWND output, HWND source, NoFocusLossSettings settings) noexcept;
	// Clears the published HWND before any output/source teardown.
	void Stop() noexcept;
	bool IsArmed() const noexcept { return _armed; }
	bool IsSpoofing() const noexcept;
	bool SuppressMessage(UINT message, WPARAM wParam) const noexcept;
	// UI-thread-only USER32 probe; no NVAPI or graphics-driver telemetry.
	NoFocusLossObservation Observe() const noexcept;
	int LastError() const noexcept { return _lastError; }

private:
	bool _armed = false;
	int _lastError = 0;
	HWND _subclassWindow = nullptr;
};
}
