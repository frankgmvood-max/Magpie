#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace Magpie {

// Optional NVAPI calls can fail inside a compatibility runtime/driver. Contain
// exceptions on the calling thread and stop entering this driver instance after
// a fault. There is deliberately no call mutex: Sleep must not block Present.
class NvapiCallGuard {
public:
	bool IsFaulted() const noexcept { return _faulted.load(std::memory_order_acquire); }
	DWORD FaultCode() const noexcept { return _faultCode.load(std::memory_order_acquire); }
	uintptr_t FaultAddress() const noexcept { return _faultAddress.load(); }
	DWORD FaultThread() const noexcept { return _faultThread.load(); }

	template<typename Function, typename Result>
	Result Invoke(Function&& function, Result failure, DWORD* sehCode) noexcept {
		*sehCode = 0;
		if (IsFaulted()) return failure;
		return _InvokeSafely(function, failure, sehCode);
	}

private:
	template<typename Function, typename Result>
	Result _InvokeSafely(Function& function, Result failure, DWORD* sehCode) noexcept {
		__try {
			return function();
		} __except (_CaptureException(GetExceptionInformation(), sehCode)) {
			return failure;
		}
	}
	LONG _CaptureException(EXCEPTION_POINTERS* exception, DWORD* sehCode) noexcept {
		const DWORD code = exception->ExceptionRecord->ExceptionCode;
		// Do not consume debugger breaks, C++ exceptions or stack exhaustion.
		if (code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP ||
			code == EXCEPTION_STACK_OVERFLOW || code == 0xE06D7363)
			return EXCEPTION_CONTINUE_SEARCH;
		*sehCode = code;
		_faulted.store(true, std::memory_order_release);
		std::scoped_lock lock(_faultMutex);
		if (!_faultCode.load()) {
			_faultAddress.store(reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress));
			_faultThread.store(GetCurrentThreadId());
			_faultCode.store(code, std::memory_order_release);
		}
		return EXCEPTION_EXECUTE_HANDLER;
	}
	std::atomic<bool> _faulted = false;
	std::atomic<DWORD> _faultCode = 0;
	std::atomic<uintptr_t> _faultAddress = 0;
	std::atomic<DWORD> _faultThread = 0;
	std::mutex _faultMutex;
};

}
