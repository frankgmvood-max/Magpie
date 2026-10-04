// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <cstdio>
#include <cwchar>
namespace ls {
// A semaphore (rather than a recursive mutex) excludes two generators even
// when they run on the very same LS render thread. Scope is this process only.
class GenerationLease {
public:
    ~GenerationLease(){Reset();if(handle_)CloseHandle(handle_);}
    bool Acquire() {
        if(owned_)return true;
        if(!handle_){wchar_t name[96];std::swprintf(name,96,L"Local\\LSExperimentalFG-%lu",GetCurrentProcessId());handle_=CreateSemaphoreW(nullptr,1,1,name);}
        owned_=handle_ && WaitForSingleObject(handle_,0)==WAIT_OBJECT_0;return owned_;
    }
    void Reset(){if(owned_){ReleaseSemaphore(handle_,1,nullptr);owned_=false;}}
private:
    HANDLE handle_=nullptr;bool owned_=false;
};
}
