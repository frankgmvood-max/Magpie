// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
namespace ls {
// A disabled, nonactivating child stays inside LS's existing output window.
// Mouse and keyboard handling stays with LS; no focus, cursor or parent style
// changes are made. Every HWND has exactly one flip-model swapchain.
class OutputWindow {
public:
    ~OutputWindow(){Reset();}
    bool Create(HWND parent,UINT width,UINT height);
    void Show(bool);
    void Reset();
    HWND Get() const;
private:
    struct State;
    State* state_=nullptr;
    static DWORD WINAPI Thread(void*);
};
}
