#pragma once
#include "include/NoFocusLossApi.h"
#include "include/NoFocusLossSettings.h"

namespace Magpie {

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
	int LastError() const noexcept { return _lastError; }

private:
	bool _armed = false;
	int _lastError = 0;
};
}
