// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
namespace sm {
struct Settings {
    bool enabled=false,nativeFGDisabled=false,preferVRR=true,filterDuplicates=true;
    bool preserveLsPresent=false,diagnostics=false,automaticRecovery=true;
    unsigned syncMode=0,devicePath=0; // VRR/LS/Vsync; Auto/Explicit/Verified default
    float targetFPS=136;
    void Validate(){syncMode=std::min(syncMode,2u);devicePath=std::min(devicePath,2u);targetFPS=targetFPS==0?0:std::clamp(targetFPS,30.f,360.f);}
};
}
