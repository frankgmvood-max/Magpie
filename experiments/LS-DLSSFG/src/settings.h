// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
namespace fg {
enum class FlowMethod : unsigned { None=0, Nvidia=2 };
enum class PresentApi : unsigned { Auto=0, Present=1, Present1=2 };
struct Settings {
    bool nativeFGDisabled=false;
    unsigned multiplier=2;
    FlowMethod flow=FlowMethod::Nvidia;
    unsigned flowQuality=2;
    bool duplicateFiltering=true;
    bool preferVRR=true;
    unsigned maximumFrameLatency=1;
    double targetFPS=136;
    PresentApi presentApi=PresentApi::Auto;
    void Validate() {
        multiplier=std::clamp(multiplier,2u,4u);
        if(flow!=FlowMethod::Nvidia) flow=FlowMethod::None;
        flowQuality=std::clamp(flowQuality,1u,5u);
        if(maximumFrameLatency>16) maximumFrameLatency=1;
        if(!std::isfinite(targetFPS) || (targetFPS!=0 && (targetFPS<30 || targetFPS>360))) targetFPS=136;
        if(unsigned(presentApi)>2) presentApi=PresentApi::Auto;
    }
};
inline unsigned SupportedMultiplier(unsigned requested,unsigned maxIntermediate) {
    return std::min(std::clamp(requested,2u,4u),std::clamp(maxIntermediate,1u,3u)+1);
}
struct EvaluationPlan {
    unsigned count;
    bool reset;
};
inline EvaluationPlan Plan(unsigned multiplier,bool reset) {
    // The reset contract is always 1/1, even when steady-state MFG is selected.
    return {reset?1u:std::clamp(multiplier,2u,4u)-1,reset};
}
}
