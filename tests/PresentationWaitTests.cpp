#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

// Minimal handle owner for compiling the actual wait helper independently of
// the application's WIL package; all waits and signals are real USER32 APIs.
namespace wil {
class unique_handle {
public:
	~unique_handle() { reset(); }
	void reset(HANDLE handle=nullptr) { if(_handle) CloseHandle(_handle); _handle=handle; }
	HANDLE get() const { return _handle; }
	explicit operator bool() const { return _handle!=nullptr; }
private:
	HANDLE _handle=nullptr;
};
}
#include "FramePacingWait.h"
using namespace Magpie;
using namespace std::chrono_literals;

int main() {
	wil::unique_handle timer;
	MSG message{};
	PeekMessageW(&message,nullptr,0,0,PM_NOREMOVE);
	for(int i=0;i<40;++i) {
		const auto due=std::chrono::steady_clock::now()+2ms;
		do { WaitForPreciseFramePacing(due,timer); } while(std::chrono::steady_clock::now()<due);
		assert(std::chrono::steady_clock::now()>=due);
	}
	// Neither precise waiting nor its short tail may consume/dispatch GUI input.
	assert(PostThreadMessageW(GetCurrentThreadId(),WM_APP+44,123,456));
	const auto due=std::chrono::steady_clock::now()+500ms;
	WaitForPreciseFramePacing(due,timer);
	assert(std::chrono::steady_clock::now()<due);
	assert(PeekMessageW(&message,nullptr,WM_APP+44,WM_APP+44,PM_REMOVE));
	assert(message.wParam==123 && message.lParam==456);
	HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);
	assert(ready);
	std::thread signal([&] { std::this_thread::sleep_for(10ms); SetEvent(ready); });
	const auto readyDue=std::chrono::steady_clock::now()+500ms;
	WaitForPreciseFramePacing(readyDue,timer,ready);
	assert(std::chrono::steady_clock::now()<readyDue);
	signal.join();
	assert(WaitForSingleObject(ready,0)==WAIT_TIMEOUT); // one owner consumed the signal
	CloseHandle(ready);
	std::cout<<"PASS: real precise timer, bounded tail, GUI input preserved and GPU-ready event interrupts wait without duplicate consumption\n";
}
