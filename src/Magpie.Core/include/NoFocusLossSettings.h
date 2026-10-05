#pragma once
#include <cstdint>

namespace Magpie {

enum class NoFocusLossMode : uint32_t {
	ForegroundOnly = 0,
	Compatibility = 1,
	ClassicCompatibility = 2
};

struct NoFocusLossSettings {
	bool enabled = true;
	NoFocusLossMode mode = NoFocusLossMode::ClassicCompatibility;
};

inline NoFocusLossMode SanitizeNoFocusLossMode(uint32_t value) noexcept {
	return value <= 2 ? static_cast<NoFocusLossMode>(value) : NoFocusLossMode::ClassicCompatibility;
}

// Classic matches the external utility's persistent foreground spoof, bounded
// by the current scaling HWND lifetime. Magpie itself always uses the genuine
// foreground bypass, including when editing parameters or switching apps.
inline bool ShouldSpoofForeground(NoFocusLossMode mode, bool validSession, bool sourceForeground,
	bool outputForeground) noexcept {
	return validSession && (mode == NoFocusLossMode::ClassicCompatibility || sourceForeground || outputForeground);
}

}
