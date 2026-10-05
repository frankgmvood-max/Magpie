#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace Magpie {

// The frontend alone owns admission. Publication slots remain FIFO-owned;
// priming never acquires a texture/latency token or blocks the message pump.
class PresentationBuffer {
public:
	using Clock = std::chrono::steady_clock;
	using Time = Clock::time_point;
	using Duration = std::chrono::nanoseconds;
	void Configure(uint32_t frames) noexcept {
		frames = std::min(frames, 2u);
		if (_frames != frames) { _frames = frames; Reset(); }
	}
	std::optional<Time> WaitUntil(Time now, uint32_t queued, Duration period,
		uint32_t multiplier, uint32_t generation) noexcept {
		if (generation != _generation) { Reset(); _generation = generation; }
		if (!queued) { Reset(); return std::nullopt; }
		if (!_frames || period <= Duration::zero() || _primed) return std::nullopt;
		if (!_first) _first = now;
		// Seed/history/runtime rejection may produce fewer than multiplier frames.
		// A static source must still become visible; never wait for an absent frame.
		const auto timeout = std::min(period * (_frames + std::clamp(multiplier, 1u, 4u)),
			Duration(std::chrono::milliseconds(100)));
		const auto giveUp = *_first + timeout;
		const auto filledAt = *_first + period * _frames;
		if (now >= giveUp || (queued > _frames && now >= filledAt)) {
			_primed = true;
			return std::nullopt;
		}
		return queued > _frames ? std::min(filledAt, giveUp) : giveUp;
	}
	void Reset() noexcept { _primed = false; _first.reset(); }
private:
	uint32_t _frames = 0, _generation = 0;
	bool _primed = false;
	std::optional<Time> _first;
};

}
