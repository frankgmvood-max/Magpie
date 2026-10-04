// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace fg {
enum class RecoveryAction { Continue, Suspend, Recreate };
// This clock observes ALL real LS Presents, including duplicates. Unique
// cadence alone cannot distinguish a static scene from a suspended renderer.
class Recovery {
public:
    void Reset(){last_=0;window_=0;suspended_=false;bad_=0;}
    RecoveryAction Observe(double now,uintptr_t window,bool visible,bool usable) {
        if(!visible || !usable){suspended_=true;last_=now;window_=window;return RecoveryAction::Suspend;}
        const bool resumed=suspended_ || (window_ && window_!=window) ||
            (last_ && (!std::isfinite(now) || now<=last_ || now-last_>0.25));
        window_=window;last_=now;suspended_=false;
        if(resumed){bad_=0;return RecoveryAction::Recreate;}
        return RecoveryAction::Continue;
    }
    bool RuntimeRejected(bool rejected){bad_=rejected?bad_+1:0;return bad_>=60;}
private:
    double last_=0;uintptr_t window_=0;bool suspended_=false;unsigned bad_=0;
};
}
