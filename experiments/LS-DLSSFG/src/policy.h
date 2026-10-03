#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fg {
// All units are seconds. No accumulating debt: a late frame rebases the
// schedule instead of sending a burst of old frames to the monitor.
class Timeline {
public:
    void Reset() { lastInput_ = 0; next_ = 0; interval_ = 0; samples_ = 0; }
    bool Observe(double now) {
        if (!lastInput_) { lastInput_ = now; return false; }
        const double dt = now - lastInput_;
        lastInput_ = now;
        if (!std::isfinite(dt) || dt < 0.001 || dt > 0.1) { Reset(); lastInput_ = now; return false; }
        interval_ = interval_ == 0 ? dt : interval_ * 0.9 + dt * 0.1;
        return ++samples_ >= 3;
    }
    double Step(double targetFps) const {
        return targetFps >= 30 && targetFps <= 240 ? 1.0 / targetFps : interval_ * 0.5;
    }
    double GeneratedDue(double now, double step) {
        if (next_ == 0 || now > next_ + step) next_ = now;
        // Only the final 2 ms may be waited here. The host owns the base cadence.
        if (next_ > now + 0.002) next_ = now;
        return std::max(now, next_);
    }
    double RealDue(double generatedPresentedAt, double step) {
        next_ = generatedPresentedAt + step * 2;
        return generatedPresentedAt + step;
    }
    double Interval() const { return interval_; }
private:
    double lastInput_ = 0, next_ = 0, interval_ = 0;
    unsigned samples_ = 0;
};
inline bool SafePresent(unsigned sync, unsigned flags) {
    // TEST, DO_NOT_SEQUENCE, RESTART, DO_NOT_WAIT, STEREO_TEMPORARY_MONO,
    // RESTRICT_TO_OUTPUT and USE_DURATION need semantics this addon can't double.
    constexpr unsigned allowed = 0x200; // DXGI_PRESENT_ALLOW_TEARING
    return sync <= 1 && (flags & ~allowed) == 0;
}
}
