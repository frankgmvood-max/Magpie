#pragma once
#include <algorithm>
#include <cstdint>

namespace Magpie {

inline uint32_t OpticalFlowAnalysisDimension(uint32_t full, uint32_t percent,
	uint32_t minimum = 1) noexcept {
	if (!full) return 0;
	percent = std::clamp(percent, 25u, 100u);
	const auto scaled = uint32_t((uint64_t(full) * percent + 99) / 100);
	return std::clamp(scaled, std::min(full, std::max(minimum, 1u)), full);
}

}
