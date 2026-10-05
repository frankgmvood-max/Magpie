#pragma once
#include <algorithm>
#include <chrono>

namespace Magpie {

// A frontend-owned phase clock. Neither Present duration nor the producer's
// queue wait is used to estimate its frequency. Small lateness is recovered
// within the VRR range; long stalls discard schedule debt instead of bursting.
class VrrPresentationClock {
public:
	using Clock = std::chrono::steady_clock;
	using Time = Clock::time_point;
	using Duration = std::chrono::nanoseconds;
	void Configure(Duration interval, Duration minimumSpacing) noexcept {
		interval = std::max(interval, Duration(1));
		minimumSpacing = std::clamp(minimumSpacing, Duration(1), interval);
		if (interval != _interval || minimumSpacing != _minimumSpacing) {
			_interval = interval;
			_minimumSpacing = minimumSpacing;
			Reset();
		}
	}
	Time Due(Time now) const noexcept { return _started ? _next : now; }
	void Submitted(Time submissionStart) noexcept {
		const auto due = Due(submissionStart);
		_next = submissionStart > due + _interval ? submissionStart + _interval :
			std::max(due + _interval, submissionStart + _minimumSpacing);
		_last = submissionStart;
		_started = true;
	}
	bool IsIdle(Time now) const noexcept {
		return !_started || now - _last >= _interval * 3;
	}
	void Reset() noexcept { _started = false; _next = {}; _last = {}; }
private:
	Duration _interval{16'666'667}, _minimumSpacing{16'666'667};
	Time _next{}, _last{};
	bool _started = false;
};

}
