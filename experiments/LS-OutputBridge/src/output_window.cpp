// SPDX-License-Identifier: MIT
#include "output_window.h"
#include <atomic>
#include <new>
namespace ls {
struct OutputWindow::State {
    HWND parent=nullptr;std::atomic<HWND> window{nullptr};UINT width=0,height=0;
    HANDLE ready=nullptr,thread=nullptr;DWORD threadId=0;
};
namespace {
LRESULT CALLBACK OutputProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    if(msg==WM_APP+7){ShowWindow(hwnd,w?SW_SHOWNOACTIVATE:SW_HIDE);return 0;}
    if(msg==WM_NCHITTEST)return HTTRANSPARENT;
    if(msg==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_PAINT){ValidateRect(hwnd,nullptr);return 0;}
    if(msg==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(hwnd,msg,w,l);
}
}
DWORD WINAPI OutputWindow::Thread(void* value) {
    auto& s=*static_cast<State*>(value);
    HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&OutputProc),&module);
    WNDCLASSW wc{};wc.hInstance=module;wc.lpfnWndProc=OutputProc;wc.lpszClassName=L"LSAddonProcessedOutput";
    RegisterClassW(&wc);
    const HWND hwnd=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT|WS_EX_NOPARENTNOTIFY,wc.lpszClassName,L"",
        WS_CHILD|WS_DISABLED|WS_CLIPSIBLINGS,0,0,int(s.width),int(s.height),s.parent,nullptr,module,nullptr);
    s.window.store(hwnd);SetEvent(s.ready);
    if(!hwnd)return 1;
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    if(IsWindow(hwnd))DestroyWindow(hwnd);s.window.store(nullptr);return 0;
}
bool OutputWindow::Create(HWND parent,UINT width,UINT height) {
    Reset();if(!parent || !IsWindow(parent) || !width || !height)return false;
    state_=new(std::nothrow) State;if(!state_)return false;
    state_->parent=parent;state_->width=width;state_->height=height;
    state_->ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!state_->ready){Reset();return false;}
    state_->thread=CreateThread(nullptr,0,Thread,state_,0,&state_->threadId);
    if(!state_->thread){Reset();return false;}
    // Window creation can synchronously query its parent's UI state. If this
    // caller owns the parent, service that window's messages while waiting;
    // otherwise a main/UI-thread initialization would deadlock its own child.
    const auto start=GetTickCount64();
    if(GetWindowThreadProcessId(parent,nullptr)==GetCurrentThreadId()){
        while(WaitForSingleObject(state_->ready,0)!=WAIT_OBJECT_0 && GetTickCount64()-start<1000){
            HANDLE handle=state_->ready;
            if(MsgWaitForMultipleObjects(1,&handle,FALSE,25,QS_ALLINPUT)==WAIT_OBJECT_0+1){
                MSG msg{};while(PeekMessageW(&msg,parent,0,0,PM_REMOVE)){
                    if(msg.message==WM_QUIT){PostQuitMessage(int(msg.wParam));Reset();return false;}
                    TranslateMessage(&msg);DispatchMessageW(&msg);
                }
            }
        }
    }else WaitForSingleObject(state_->ready,1000);
    if(WaitForSingleObject(state_->ready,0)!=WAIT_OBJECT_0 || !Get()){Reset();return false;}
    return true;
}
HWND OutputWindow::Get()const{return state_?state_->window.load():nullptr;}
void OutputWindow::Show(bool show){
    if(const HWND hwnd=Get()){
        if(((GetWindowLongPtrW(hwnd,GWL_STYLE)&WS_VISIBLE)!=0)==show)return;
        DWORD_PTR result=0;
        // Visibility is established before the first inference Present. A
        // hidden DXGI output can return OCCLUDED and never execute the model.
        // The output thread is responsive; a bounded send avoids UI deadlocks.
        if(!SendMessageTimeoutW(hwnd,WM_APP+7,show,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&result))ShowWindowAsync(hwnd,show?SW_SHOWNOACTIVATE:SW_HIDE);
    }
}
void OutputWindow::Reset() {
    if(!state_)return;
    if(const HWND hwnd=Get()){ShowWindowAsync(hwnd,SW_HIDE);PostMessageW(hwnd,WM_CLOSE,0,0);}
    else if(state_->threadId)PostThreadMessageW(state_->threadId,WM_QUIT,0,0);
    if(state_->thread && WaitForSingleObject(state_->thread,1000)!=WAIT_OBJECT_0) {
        // The pinned module and heap state remain valid if the window thread
        // is blocked inside the OS. Never force-kill a window-owning thread.
        state_=nullptr;return;
    }
    if(state_->thread)CloseHandle(state_->thread);if(state_->ready)CloseHandle(state_->ready);delete state_;state_=nullptr;
}
}
