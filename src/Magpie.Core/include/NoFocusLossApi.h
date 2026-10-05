#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <atomic>

namespace Magpie {
namespace NoFocusLossDetail {
	using ForegroundGetter = HWND(WINAPI*)();
	// Retained for the entire process lifetime: disabling a hook must not free
	// a trampoline which another thread may still be executing.
	inline std::atomic<ForegroundGetter> genuineForegroundGetter{ &::GetForegroundWindow };
}

inline HWND GetActualForegroundWindow() noexcept {
	return NoFocusLossDetail::genuineForegroundGetter.load(std::memory_order_acquire)();
}
}
