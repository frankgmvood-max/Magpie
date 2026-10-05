#pragma once
#include <cstdint>

namespace Magpie {

enum class NoFocusLossMode : uint32_t { ForegroundOnly = 0, Compatibility = 1 };

struct NoFocusLossSettings {
	bool enabled = true;
	NoFocusLossMode mode = NoFocusLossMode::Compatibility;
};

inline NoFocusLossMode SanitizeNoFocusLossMode(uint32_t value) noexcept {
	return value <= 1 ? static_cast<NoFocusLossMode>(value) : NoFocusLossMode::Compatibility;
}

// An unrelated foreground application (including Magpie's settings/input
// window) always receives genuine focus information. Resuming the source game
// resumes compatibility without reinjection or a stored foreground HWND.
inline bool ShouldSpoofForeground(bool validSession, bool sourceForeground,
	bool outputForeground) noexcept {
	return validSession && (sourceForeground || outputForeground);
}

}
