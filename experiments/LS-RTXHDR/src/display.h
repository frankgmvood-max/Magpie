// SPDX-License-Identifier: GPL-3.0-only
// Adapted from Magpie 0.6.9 HdrDisplayPreflight.h; query only, never changes
// the system HDR toggle or another monitor's display settings.
#pragma once
#include <windows.h>
#include <vector>
namespace hdr {
inline bool DisplayHdr(HWND window){
    MONITORINFOEXW info{};info.cbSize=sizeof info;
    if(!window || !GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info))return false;
    for(unsigned retry=0;retry<3;++retry){
        UINT32 pc=0,mc=0;if(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pc,&mc)!=ERROR_SUCCESS)return false;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pc);std::vector<DISPLAYCONFIG_MODE_INFO> modes(mc);
        const LONG hr=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pc,paths.data(),&mc,modes.data(),nullptr);
        if(hr==ERROR_INSUFFICIENT_BUFFER)continue;if(hr!=ERROR_SUCCESS)return false;
        for(UINT32 i=0;i<pc;++i){
            const auto& path=paths[i];DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof source,path.sourceInfo.adapterId,path.sourceInfo.id};
            if(DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS || _wcsicmp(source.viewGdiDeviceName,info.szDevice))continue;
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 current{};
            current.header={DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2,sizeof current,path.targetInfo.adapterId,path.targetInfo.id};
            if(DisplayConfigGetDeviceInfo(&current.header)==ERROR_SUCCESS)return current.activeColorMode==DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
            DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO previous{};
            previous.header={DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,sizeof previous,path.targetInfo.adapterId,path.targetInfo.id};
            return DisplayConfigGetDeviceInfo(&previous.header)==ERROR_SUCCESS && previous.advancedColorEnabled;
        }
        return false;
    }
    return false;
}
}
