#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Magpie {

enum class VrrOutputMode : uint32_t { Composition = 0, Hwnd = 1 };

inline float SanitizeVrrFrameRate(float rate) noexcept {
	return std::isfinite(rate) && rate >= 30.0f && rate <= 360.0f ? rate : 0.0f;
}

// Final output rate, including generated and overlay-only frames. Leave room
// below the display ceiling for scheduler jitter and driver presentation work.
inline double ResolveVrrCeiling(double requested, double refresh) noexcept {
	if (!std::isfinite(refresh) || refresh < 24 || refresh > 1000) refresh = 60;
	const double automatic = std::max(1.0, std::floor(refresh * 0.95));
	return requested >= 30 && requested <= 360 && std::isfinite(requested)
		? std::min(requested, automatic) : automatic;
}

inline double ResolveVrrOutputRate(double requested, double refresh,
	double baseTarget, uint32_t multiplier) noexcept {
	const double ceiling = ResolveVrrCeiling(requested, refresh);
	return std::isfinite(baseTarget) && baseTarget > 0
		? std::min(ceiling, baseTarget * std::clamp(multiplier, 1u, 4u)) : ceiling;
}

}
