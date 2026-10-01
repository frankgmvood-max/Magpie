#include "FramePresentationTiming.h"
#include <cassert>
#include <iostream>
using namespace std::chrono_literals;
using Magpie::FramePresentationClock;
int main() {
    using Clock = FramePresentationClock::Clock;
    const auto period = std::chrono::nanoseconds(7'352'941);
    FramePresentationClock pacing;
    const auto origin = Clock::time_point(1s);
    pacing.PresentedUniform(origin, origin, period);
    auto previous = origin;
    // Ordinary small timer errors must not accumulate into cadence drift.
    for (int i = 1; i <= 1000; ++i) {
        auto due = pacing.Due(previous, period);
        assert(due == origin + period * i);
        auto submitted = due + 20us;
        pacing.PresentedUniform(submitted, due, period);
        previous = submitted;
    }
    auto due = pacing.Due(previous, period);
    // An expensive frame cannot be followed immediately by a catch-up burst.
    auto delayed = due + period * 3 / 4;
    pacing.PresentedUniform(delayed, due, period);
    assert(pacing.Due(delayed, period) - delayed >= period * 19 / 20);
    due = pacing.Due(delayed, period);
    auto stalled = due + 500ms;
    pacing.PresentedUniform(stalled, due, period);
    assert(pacing.Due(stalled, period) == stalled + period);
    pacing.Reset();
    assert(pacing.Due(stalled, period) == stalled);
    std::cout << "PASS: fixed phase, bounded catch-up, long-stall recovery, reset\n";
}
