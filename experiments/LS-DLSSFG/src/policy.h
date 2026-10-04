#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace fg {
// Unique-input cadence. It includes backpressure from the synchronous addon;
// it is not a measurement of the game's original render cadence or scanout.
class Timeline {
public:
    void Reset() { lastInput_ = 0; interval_ = 0; samples_ = 0; }
    bool Observe(double now) {
        if (!lastInput_) { lastInput_ = now; return false; }
        const double dt = now - lastInput_;
        lastInput_ = now;
        if (!std::isfinite(dt) || dt < 0.001 || dt > 0.1) { Reset(); lastInput_ = now; return false; }
        interval_ = interval_ == 0 ? dt : interval_ * 0.9 + dt * 0.1;
        return ++samples_ >= 3;
    }
    bool NeedsReset(double now) const {return !lastInput_ || now-lastInput_>0.1 || now<=lastInput_;}
    double Step(double targetFps,unsigned multiplier=2) const {
        return targetFps >= 30 && targetFps <= 360 ? 1.0 / targetFps : interval_ / std::clamp(multiplier,2u,4u);
    }
    double FrameDue(double first,unsigned index,double step) const {return first+index*step;}
    double Interval() const { return interval_; }
private:
    double lastInput_ = 0, interval_ = 0;
    unsigned samples_ = 0;
};
// One minimum interval for EVERY output, including history and duplicates.
// Anchor to the beginning of the actual DXGI call, not its return: blocking
// inside Present must not be charged again to the next scheduled interval.
// No catch-up burst, no early-deadline truncation and no invented extra frames.
class OutputPacer {
public:
    void Reset(){last_=0;}
    double Due(double ready,double step) const {
        return last_>0 && std::isfinite(step) && step>0?std::max(ready,last_+step):ready;
    }
    void Submitted(double begin){if(std::isfinite(begin) && begin>last_)last_=begin;}
private:
    double last_=0;
};
struct SubmissionStats {
    uint64_t count=0,gaps=0;
    double last=0,sumGap=0,minGap=0,maxGap=0,maxPresent=0,maxLate=0;
    void Observe(double begin,double end,double due){
        ++count;
        if(last>0 && begin>last){
            const double gap=begin-last;
            minGap=gaps?std::min(minGap,gap):gap;maxGap=std::max(maxGap,gap);sumGap+=gap;++gaps;
        }
        last=begin;maxPresent=std::max(maxPresent,std::max(0.0,end-begin));
        if(due>0)maxLate=std::max(maxLate,std::max(0.0,begin-due));
    }
    double MeanGap() const{return gaps?sumGap/gaps:0;}
    void ClearWindow(){const double previous=last;*this={};last=previous;}
};
inline bool SafePresent(unsigned sync, unsigned flags) {
    // TEST, DO_NOT_SEQUENCE, RESTART, DO_NOT_WAIT, STEREO_TEMPORARY_MONO,
    // RESTRICT_TO_OUTPUT and USE_DURATION need semantics this addon can't double.
    constexpr unsigned allowed = 0x200; // DXGI_PRESENT_ALLOW_TEARING
    return sync <= 1 && (flags & ~allowed) == 0;
}
struct PresentMode {
    unsigned sync, flags;
    bool vrrRequested;
};
inline PresentMode ChoosePresent(unsigned sync, unsigned flags, bool preferVRR,
                                 bool tearingChain, bool windowed) {
    // ALLOW_TEARING is legal only on a chain created with that flag, with
    // sync=0 and outside fullscreen exclusive. It cannot be added by resizing.
    if (preferVRR && tearingChain && windowed && SafePresent(sync, flags))
        return {0, flags | 0x200, true};
    return {sync, flags, false};
}
}
