#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace Magpie {

inline constexpr uint32_t DLSS_PRESENTATION_MAX_SLOTS = 6;

struct DlssPresentationSettings {
	uint32_t maximumFrameLatency = 2;
	uint32_t bufferFrames = 1;
	bool strictPacing = true;
	bool gpuReady = true;
};

// Configuration/UI floats must be validated before integer conversion.
template<class GetValue>
DlssPresentationSettings ReadDlssPresentationSettings(GetValue&& getValue) noexcept {
	auto choice = [&](std::string_view name, int low, int high, int fallback) {
		const auto value = getValue(name);
		return value && std::isfinite(*value) && *value >= float(low) &&
			*value <= float(high) && std::round(*value) == *value
			? static_cast<int>(*value) : fallback;
	};
	return {
		static_cast<uint32_t>(choice("maximumFrameLatency", 1, 3, 2)),
		static_cast<uint32_t>(choice("presentationBufferFrames", 0, 2, 1)),
		choice("presentationPacing", 0, 1, 0) == 0,
		choice("presentationGpuReady", 0, 1, 1) != 0
	};
}

inline uint32_t DlssPresentationSlotCount(uint32_t multiplier, uint32_t bufferFrames) noexcept {
	return std::clamp(multiplier, 2u, 4u) + std::min(bufferFrames, 2u);
}

inline uint32_t DlssSwapChainBufferCount(uint32_t maximumFrameLatency) noexcept {
	return std::max(3u, std::clamp(maximumFrameLatency, 1u, 3u) + 1u);
}

inline bool IsDlssPresentationParameter(std::string_view name) noexcept {
	return name == "maximumFrameLatency" || name == "presentationBufferFrames" ||
		name == "presentationPacing" || name == "presentationGpuReady";
}

}
