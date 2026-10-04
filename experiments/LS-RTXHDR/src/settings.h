// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
namespace hdr {
struct Settings {
    bool enabled=false,nativeHdrDisabled=false,duplicateFiltering=true;
    unsigned contrast=100,saturation=100,middleGray=50,peakNits=1000;
    unsigned outputMode=0; // Auto, SDR display tone map, HDR scRGB
    float sdrWhiteNits=80,exposure=1,shoulder=1;
    void Validate(){
        contrast=std::min(contrast,200u);saturation=std::min(saturation,200u);
        middleGray=std::clamp(middleGray,10u,100u);peakNits=std::clamp(peakNits,400u,2000u);
        outputMode=std::min(outputMode,2u);
        sdrWhiteNits=std::clamp(sdrWhiteNits,40.f,300.f);exposure=std::clamp(exposure,0.1f,4.f);shoulder=std::clamp(shoulder,0.1f,4.f);
    }
};
}
