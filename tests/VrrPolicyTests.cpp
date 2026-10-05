#include "include/VrrSettings.h"
#include "VrrPresentationClock.h"
#include "FramePresentationTiming.h"
#include "OpticalFlowResolution.h"
#include "OpticalFlowHistory.h"
#include "MotionVectorRequest.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace Magpie;
using namespace std::chrono_literals;

int main() {
	assert(ResolveVrrCeiling(0, 144) == 136);
	assert(ResolveVrrCeiling(144, 144) == 136);
	assert(ResolveVrrCeiling(120, 144) == 120);
	assert(ResolveVrrCeiling(0, 60) == 57);
	assert(ResolveVrrCeiling(0, std::numeric_limits<double>::quiet_NaN()) == 57);
	assert(SanitizeVrrFrameRate(std::numeric_limits<float>::infinity()) == 0);
	for (unsigned mult = 1; mult <= 4; ++mult) {
		assert(ResolveVrrOutputRate(136, 144, 136.0 / mult, mult) == 136);
		assert(ResolveVrrOutputRate(136, 144, 30, mult) == 30 * mult);
	}
	const auto period = std::chrono::nanoseconds(1'000'000'000 / 136);
	const auto minimum = std::chrono::nanoseconds(1'000'000'000 / 142);
	VrrPresentationClock clock;
	clock.Configure(period, minimum);
	VrrPresentationClock::Time now{};
	clock.Submitted(now);
	// One million presentations with variable CPU/Present duration. A small
	// late submission may recover phase, but never cross the VRR ceiling.
	for (int i = 1; i <= 1'000'000; ++i) {
		const auto due = clock.Due(now);
		assert(due - now >= minimum);
		const auto next = due + (i % 17 == 0 ? 150us : 0us);
		clock.Submitted(next);
		now = next;
		assert(clock.Due(now) == VrrPresentationClock::Time(period * (i + 1)));
	}
	assert(!clock.IsIdle(now + period));
	assert(clock.IsIdle(now + period * 3));
	const auto stall = now + 2s;
	clock.Submitted(stall);
	assert(clock.Due(stall) == stall + period);
	clock.Configure(10ms, 9ms);
	assert(clock.Due(stall) == stall);
	clock.Submitted(stall);
	assert(clock.Due(stall) == stall + 10ms);
	clock.Reset();
	assert(clock.Due(now) == now);
	// Strict mode deliberately does not repay late CPU/GPU work with a short
	// following interval. This differs observably from the retained VRR5 mode.
	clock.Configure(period, period);
	now = VrrPresentationClock::Time{};
	clock.Submitted(now);
	for (int i = 1; i <= 1'000'000; ++i) {
		const auto due = clock.Due(now);
		assert(due - now == period);
		const auto next = due + (i % 17 == 0 ? 150us : 0us);
		clock.Submitted(next);
		assert(clock.Due(next) == next + period);
		now = next;
	}
	clock.Submitted(now + 2s);
	assert(clock.Due(now + 2s) == now + 2s + period);

	CaptureFrameCadence capture;
	VrrPresentationClock::Time delivery{};
	int64_t timestamp = 1'000'000;
	capture.Observe(delivery, 0ns, timestamp);
	for (int i = 0; i < 120; ++i) {
		delivery += 25ms; // Producer is backpressured, source remains 50 FPS.
		timestamp += 200'000;
		capture.Observe(delivery, 3ms, timestamp);
	}
	assert(capture.Interval(2, 0) == 10ms);
	assert(capture.Observe(delivery + 1s, 0ns, timestamp + 10'000'000));
	capture.RestartSequence();
	capture.Observe(delivery + 2s, 0ns, std::numeric_limits<int64_t>::max());

	for (uint32_t size : {1u, 63u, 1921u, 3440u, 1440u}) {
		for (uint32_t percent : {25u, 50u, 75u, 100u}) {
			const auto reduced = OpticalFlowAnalysisDimension(size, percent);
			assert(reduced >= 1 && reduced <= size);
			assert(reduced == (uint64_t(size) * percent + 99) / 100);
		}
	}
	assert(OpticalFlowAnalysisDimension(0, 50) == 0);
	assert(OpticalFlowAnalysisDimension(3440, 25, 1024) == 1024);
	OpticalFlowHistory history;
	assert(history.DisableTemporalHints()); // Seeding must retain this state.
	history.Executed();
	assert(!history.DisableTemporalHints());
	history.Reset();
	assert(history.DisableTemporalHints());
	FrameGuidanceRequirements requirements;
	const auto low = MotionVectorRequest::Nvidia(NvidiaOpticalFlowQuality::Quality, 25);
	const auto high = MotionVectorRequest::Nvidia(NvidiaOpticalFlowQuality::Balanced, 75);
	requirements.Add(low); requirements.Add(high);
	const auto resolved = requirements.Resolved();
	assert(resolved.PreferredMotion().resolutionPercent == 75);
	assert(requirements.Contains(low) && requirements.Contains(high));
	assert(resolved.Contains(low));
	assert(FrameGuidanceRequirements::ResolveConsumer(high, resolved.PreferredMotion()).resolutionPercent == 75);
	assert(!resolved.Contains(MotionVectorRequest::Nvidia(NvidiaOpticalFlowQuality::Quality, 100)));
	std::cout << "PASS: VRR final limits x1-x4, one million phase and one million strict submissions, bounded spacing, stalls, capture timestamps, OF dimensions, sharing and temporal resets\n";
}
